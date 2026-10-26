/* regex.c -- std::regex as Nix 2.34 runs it, for builtins.match and
 * builtins.split (docs/REGEX.md).
 *
 * Nix compiles a pattern with std::regex(re, std::regex::extended) and runs
 * std::regex_match, or a std::cregex_iterator, over the string, in the "C"
 * locale. Its answers -- which match, which captures, which patterns are
 * errors -- are libstdc++'s, and rest on details no POSIX text gives: the
 * depth-first executor stops trying a loop's exit once the loop has found a
 * match, tries both arms of every |, keeps the first of the longest
 * solutions, and never resets a capture between iterations. So this is a
 * port, structure for structure, of the libstdc++ the host's Nix links
 * (GCC 15.3): bits/regex_scanner.tcc, regex_compiler.tcc and
 * regex_automaton.tcc build the same NFA -- dummies, clones and orphans
 * included -- and the executor is regex_executor.tcc's depth-first one (what
 * regex_match and regex_search run without the __polynomial flag), its
 * recursion turned into an explicit stack of undo records.
 *
 * Derived from the GNU ISO C++ Library:
 *   Copyright (C) 2010-2025 Free Software Foundation, Inc.
 *   GPL-3.0-or-later, with the GCC Runtime Library Exception, version 3.1.
 *
 * Nothing here allocates or runs bytecode. The NFA, the captures, split's
 * matches and the executor's stack live in bank 7 (REGEX_BASE); positions
 * are 16-bit offsets into the subject; the pattern and the subject are views
 * the caller holds. An error is rx_fail, the caller's, which does not return.
 *
 * Bank 7, from the bottom: this context; the parser's open groups; the
 * states, 8 bytes each, growing up; after compiling, the executor's
 * captures and then the records, one per match; and from the top down, the
 * brackets' 32-byte bitmaps and below them the executor's (or a clone's)
 * stack.
 */
#pragma noroot
#pragma optimize 15

#include <string.h>
#include "regex.h"

/* A segment of its own: bank 6, after float and json (VM.md). */
segment "regex";

#ifdef RX_HOST
extern uint8_t *rx_mem;                 /* 64 KB of the host's, for testing it there */
#define RX_P(off) (rx_mem + (uint16_t)(off))
#else
#define RX_P(off) ((uint8_t *)(REGEX_BASE | (uint32_t)(uint16_t)(off)))
#endif
#define RXW(off)  (*(uint16_t *)RX_P(off))
#define RXB(off)  (*RX_P(off))
#define W(p)      (*(uint16_t *)(p))

#define NONE      0xFFFFU               /* _S_invalid_state_id; no position */

typedef struct {
    const uint8_t *re;      /* the pattern */
    const uint8_t *s;       /* the subject */
    uint16_t rn, rp;        /* the pattern's length; the scanner's position */
    uint16_t sn;            /* the subject's length */
    uint16_t nst;           /* states */
    uint16_t ngroups;       /* _M_subexpr_count */
    uint16_t hi;            /* the lowest bitmap (or stack entry) */
    uint16_t lo;            /* the executor's floor: above the records */
    uint16_t depth;         /* open groups */
    uint16_t voff, vlen;    /* the scanner's value: a slice of the pattern */
    uint16_t cvoff, cvlen;  /* the compiler's (_M_value) */
    uint16_t cf, cs, cm;    /* the executor's captures: firsts, seconds, matched */
    uint16_t recs, recsize; /* the records; one is cf..cm's 5 bytes a group */
    uint16_t best;          /* the record the executor writes (_M_results) */
    uint8_t  tok, sstate, at_bstart, vch, cvch;
} rx_t;
#define RX ((rx_t *)RX_P(0))

/* The parser's open groups: the group's SUB_BEGIN, its disjunction so far
 * and the alternative being built (start, end; NONE when empty). */
#define RX_FRAMES   0x0080
#define RX_DEPTH    128
#define FR_OPEN(f)  RXW((f) + 0)
#define FR_DS(f)    RXW((f) + 2)
#define FR_DE(f)    RXW((f) + 4)
#define FR_AS(f)    RXW((f) + 6)
#define FR_AE(f)    RXW((f) + 8)
#define RX_STATES   (RX_FRAMES + 16 * RX_DEPTH)
#define RX_TOP      0xFFE0U

/* A state: _State's opcode, _M_next, and _M_alt or the matcher or the group
 * in one word; a REPEAT's _M_rep_count too (and, while cloning, the copy). */
typedef struct {
    uint8_t  op;
    uint8_t  cnt;       /* REPEAT: _M_rep_count.second, 0..2 */
    uint16_t next;
    uint16_t x;         /* ALT, REPEAT: _M_alt; CHAR: the byte; SET: the bitmap; SUB_*: the group */
    uint16_t rpos;      /* REPEAT: _M_rep_count.first */
} rx_state;
#define ST(i)       ((rx_state *)RX_P(RX_STATES + ((uint16_t)(i) << 3)))

#define O_DUMMY     0
#define O_ALT       1   /* POSIX: both arms, the alt first */
#define O_REPEAT    2   /* greedy: once more (the alt), then -- if nothing matched -- the next */
#define O_SUB_BEGIN 3
#define O_SUB_END   4
#define O_BOL       5
#define O_EOL       6
#define O_ANY       7
#define O_CHAR      8
#define O_SET       9
#define O_ACCEPT    10
#define HAS_ALT(op) ((op) == O_ALT || (op) == O_REPEAT)

/* _ScannerBase's tokens, those the extended grammar makes */
#define T_EOF           0
#define T_ORD           1
#define T_SUB_BEGIN     2
#define T_SUB_END       3
#define T_BRACKET_BEGIN 4
#define T_BRACKET_NEG   5
#define T_BRACKET_END   6
#define T_DASH          7
#define T_INTERVAL      8
#define T_INTERVAL_END  9
#define T_COMMA         10
#define T_DUP           11
#define T_COLLSYM       12
#define T_EQUIV         13
#define T_CLASS         14
#define T_LINE_BEGIN    15
#define T_LINE_END      16
#define T_ANY           17
#define T_STAR          18
#define T_PLUS          19
#define T_OPT           20
#define T_OR            21

#define S_NORMAL        0
#define S_BRACKET       1
#define S_BRACE         2

/* ---- the scanner (regex_scanner.tcc), POSIX extended ---- */

/* _M_extended_spec_char: ".[\\()*+?{|^$". A NUL is never one. */
static int is_spec(uint8_t ch)
{
    return ch == '.' || ch == '[' || ch == '\\' || ch == '(' || ch == ')' || ch == '*'
        || ch == '+' || ch == '?' || ch == '{' || ch == '|' || ch == '^' || ch == '$';
}

/* _M_eat_class: the name up to d, then d and ']' */
static void eat_class(uint8_t d)
{
    rx_t *c = RX;
    c->voff = c->rp;
    while (c->rp != c->rn && c->re[c->rp] != d) ++c->rp;
    c->vlen = c->rp - c->voff;
    if (c->rp == c->rn) rx_fail(RX_EINVAL);
    ++c->rp;
    if (c->rp == c->rn || c->re[c->rp] != ']') rx_fail(RX_EINVAL);
    ++c->rp;
}

/* _M_advance: the next token, one ahead of the compiler. The end of the
 * pattern is EOF in every state. */
static void advance(void)
{
    rx_t *c = RX;
    uint8_t ch, d;
    if (c->rp == c->rn) { c->tok = T_EOF; return; }
    ch = c->re[c->rp++];
    if (c->sstate == S_BRACKET) {                   /* _M_scan_in_bracket */
        if (ch == '-') c->tok = T_DASH;
        else if (ch == '[') {
            if (c->rp == c->rn) rx_fail(RX_EINVAL);
            d = c->re[c->rp];
            if (d == '.' || d == ':' || d == '=') {
                if (d == '.') c->tok = T_COLLSYM;
                else if (d == ':') c->tok = T_CLASS;
                else c->tok = T_EQUIV;
                ++c->rp;
                eat_class(d);
            } else { c->tok = T_ORD; c->vch = ch; }
        } else if (ch == ']' && !c->at_bstart) {    /* "[]" and "[^]": the ']' is a character */
            c->tok = T_BRACKET_END;
            c->sstate = S_NORMAL;
        } else { c->tok = T_ORD; c->vch = ch; }     /* a backslash too, in POSIX */
        c->at_bstart = 0;
        return;
    }
    if (c->sstate == S_BRACE) {                     /* _M_scan_in_brace */
        if (ch >= '0' && ch <= '9') {
            c->tok = T_DUP;
            c->voff = c->rp - 1;
            while (c->rp != c->rn && c->re[c->rp] >= '0' && c->re[c->rp] <= '9') ++c->rp;
            c->vlen = c->rp - c->voff;
        } else if (ch == ',') c->tok = T_COMMA;
        else if (ch == '}') { c->sstate = S_NORMAL; c->tok = T_INTERVAL_END; }
        else rx_fail(RX_EINVAL);
        return;
    }
    switch (ch) {                                   /* _M_scan_normal */
    case '\\':
        /* _M_eat_escape_posix, built strict (__STRICT_ANSI__, as Nix is):
         * only a special character may be escaped -- \d \] \n are errors */
        if (c->rp == c->rn) rx_fail(RX_EINVAL);
        ch = c->re[c->rp++];
        if (!is_spec(ch)) rx_fail(RX_EINVAL);
        c->tok = T_ORD; c->vch = ch;
        return;
    case '(': c->tok = T_SUB_BEGIN; return;
    case ')': c->tok = T_SUB_END; return;
    case '[':
        c->sstate = S_BRACKET;
        c->at_bstart = 1;
        if (c->rp != c->rn && c->re[c->rp] == '^') { c->tok = T_BRACKET_NEG; ++c->rp; }
        else c->tok = T_BRACKET_BEGIN;
        return;
    case '{': c->sstate = S_BRACE; c->tok = T_INTERVAL; return;
    case '^': c->tok = T_LINE_BEGIN; return;
    case '$': c->tok = T_LINE_END; return;
    case '.': c->tok = T_ANY; return;
    case '*': c->tok = T_STAR; return;
    case '+': c->tok = T_PLUS; return;
    case '?': c->tok = T_OPT; return;
    case '|': c->tok = T_OR; return;
    case 0: rx_fail(RX_EINVAL);                     /* _S_null */
    default: c->tok = T_ORD; c->vch = ch; return;   /* ']' and '}' among them */
    }
}

/* _M_match_token: take the token and its value, and scan the next */
static int match_tok(uint8_t t)
{
    rx_t *c = RX;
    if (c->tok != t) return 0;
    c->cvch = c->vch;
    c->cvoff = c->voff;
    c->cvlen = c->vlen;
    advance();
    return 1;
}

/* ---- the NFA (regex_automaton.tcc) ---- */

/* _M_insert_state. Nix's limit is 100,000 states (error_space, "memory limit
 * exceeded"); the bank's is lower, and is E_NOMEM too. */
static uint16_t insert(uint8_t op, uint16_t next, uint16_t x)
{
    rx_t *c = RX;
    uint16_t id = c->nst;
    rx_state *st;
    if (id >= (uint16_t)((uint16_t)(c->hi - RX_STATES) >> 3)) rx_fail(RX_ESPACE);
    st = ST(id);
    st->op = op;
    st->cnt = 0;
    st->next = next;
    st->x = x;
    st->rpos = NONE;
    c->nst = id + 1;
    return id;
}

/* _StateSeq::_M_clone: a copy of the states from s that the end's next does
 * not lead out of. While it runs, each state copied holds its copy in rpos,
 * and each copy its original. (libstdc++ can copy a state twice, when two
 * paths push it before it is popped; the first copy is unreachable, so only
 * its count of states differs.) */
static void clone(uint16_t s, uint16_t e, uint16_t *cs, uint16_t *ce)
{
    rx_t *c = RX;
    uint16_t top = c->hi, first = c->nst, u, n, t;
    rx_state *st, *ns;
    if ((uint16_t)(c->hi - RX_STATES) < (uint16_t)((c->nst << 3) + 2)) rx_fail(RX_ESPACE);
    c->hi -= 2;
    RXW(c->hi) = s;
    while (c->hi != top) {
        u = RXW(c->hi);
        c->hi += 2;
        st = ST(u);
        if (st->rpos != NONE) continue;
        n = insert(st->op, st->next, st->x);
        st = ST(u);
        st->rpos = n;
        ST(n)->rpos = u;
        if (HAS_ALT(st->op) && st->x != NONE && ST(st->x)->rpos == NONE) {
            if ((uint16_t)(c->hi - RX_STATES) < (uint16_t)((c->nst << 3) + 2)) rx_fail(RX_ESPACE);
            c->hi -= 2;
            RXW(c->hi) = st->x;
        }
        if (u == e) continue;
        if (st->next != NONE && ST(st->next)->rpos == NONE) {
            if ((uint16_t)(c->hi - RX_STATES) < (uint16_t)((c->nst << 3) + 2)) rx_fail(RX_ESPACE);
            c->hi -= 2;
            RXW(c->hi) = st->next;
        }
    }
    for (n = first; n != c->nst; ++n) {
        ns = ST(n);
        if (ns->next != NONE) ns->next = ST(ns->next)->rpos;
        if (HAS_ALT(ns->op) && ns->x != NONE) ns->x = ST(ns->x)->rpos;
    }
    *cs = ST(s)->rpos;
    *ce = ST(e)->rpos;
    for (n = first; n != c->nst; ++n) {
        ns = ST(n);
        t = ns->rpos;
        ST(t)->rpos = NONE;
        ns->rpos = NONE;
    }
}

/* _M_eliminate_dummy */
static void eliminate_dummy(void)
{
    rx_t *c = RX;
    uint16_t i;
    rx_state *st;
    for (i = 0; i != c->nst; ++i) {
        st = ST(i);
        while (st->next != NONE && ST(st->next)->op == O_DUMMY) st->next = ST(st->next)->next;
        if (HAS_ALT(st->op))
            while (st->x != NONE && ST(st->x)->op == O_DUMMY) st->x = ST(st->x)->next;
    }
}

/* ---- brackets (_BracketMatcher, built once into a bitmap of the 256 bytes) ---- */

#define BIT(b)      ("\001\002\004\010\020\040\100\200"[(b) & 7])

static void setbit(uint16_t bm, uint8_t b)
{
    RXB(bm + (b >> 3)) |= BIT(b);
}

/* regex_traits::lookup_collatename's names, char 0 to 127 in order */
#define COLLATE_NAMES \
    "NUL SOH STX ETX EOT ENQ ACK alert backspace tab newline vertical-tab " \
    "form-feed carriage-return SO SI DLE DC1 DC2 DC3 DC4 NAK SYN ETB CAN EM SUB " \
    "ESC IS4 IS3 IS2 IS1 space exclamation-mark quotation-mark number-sign " \
    "dollar-sign percent-sign ampersand apostrophe left-parenthesis " \
    "right-parenthesis asterisk plus-sign comma hyphen period slash zero one " \
    "two three four five six seven eight nine colon semicolon less-than-sign " \
    "equals-sign greater-than-sign question-mark commercial-at A B C D E F G H " \
    "I J K L M N O P Q R S T U V W X Y Z left-square-bracket backslash " \
    "right-square-bracket circumflex underscore grave-accent a b c d e f g h i " \
    "j k l m n o p q r s t u v w x y z left-curly-bracket vertical-line " \
    "right-curly-bracket tilde DEL "
/* lookup_classname's, after the name is lowercased; the index is inclass's k */
#define CLASS_NAMES \
    "d w s alnum alpha blank cntrl digit graph lower print punct space upper xdigit "

/* The index of the compiler's value in a table of space-ended names, or
 * NONE; fold lowercases the value first. */
static uint16_t lookup(const char *tab, uint8_t fold)
{
    rx_t *c = RX;
    uint16_t idx = 0, j, len = c->cvlen;
    const uint8_t *v = c->re + c->cvoff;
    uint8_t ch;
    while (*tab) {
        for (j = 0; tab[j] != ' '; ++j) ;
        if (j == len) {
            for (j = 0; j != len; ++j) {
                ch = v[j];
                if (fold && ch >= 'A' && ch <= 'Z') ch += 32;
                if ((uint8_t)tab[j] != ch) break;
            }
            if (j == len) return idx;
            for (j = 0; tab[j] != ' '; ++j) ;
        }
        tab += j + 1;
        ++idx;
    }
    return NONE;
}

/* The "C" locale's ctype (glibc's table): nothing above 127 is in a class. */
static int inclass(uint16_t k, uint8_t ch)
{
    int up = ch >= 'A' && ch <= 'Z', lo = ch >= 'a' && ch <= 'z', dg = ch >= '0' && ch <= '9';
    switch (k) {
    case 0: case 7:  return dg;                                     /* d digit */
    case 1:          return up || lo || dg || ch == '_';            /* w: alnum and _ */
    case 2: case 12: return ch == ' ' || (ch >= 9 && ch <= 13);     /* s space */
    case 3:          return up || lo || dg;                         /* alnum */
    case 4:          return up || lo;                               /* alpha */
    case 5:          return ch == ' ' || ch == '\t';                /* blank */
    case 6:          return ch < 32 || ch == 127;                   /* cntrl */
    case 8:          return ch > 32 && ch < 127;                    /* graph */
    case 9:          return lo;                                     /* lower */
    case 10:         return ch >= 32 && ch < 127;                   /* print */
    case 11:         return ch > 32 && ch < 127 && !(up || lo || dg);   /* punct */
    case 13:         return up;                                     /* upper */
    default:         return dg || (ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f');   /* xdigit */
    }
}

/* _M_make_range: char is signed, so a range from below 128 to above it is
 * an error, and one above 127 holds only bytes above 127. */
static void range(uint16_t bm, uint8_t l, uint8_t r)
{
    uint16_t lo = (uint8_t)(l ^ 0x80), hi = (uint8_t)(r ^ 0x80), v;
    if (lo > hi) rx_fail(RX_EINVAL);
    for (v = lo; v <= hi; ++v) setbit(bm, (uint8_t)(v ^ 0x80));
}

#define L_NONE  0
#define L_CHAR  1
#define L_CLASS 2

/* _BracketState's push_char and push_class */
static void push_char(uint16_t bm, uint8_t *lt, uint8_t *last, uint8_t ch)
{
    if (*lt == L_CHAR) setbit(bm, *last);
    *lt = L_CHAR;
    *last = ch;
}

static void push_class(uint16_t bm, uint8_t *lt, uint8_t *last)
{
    if (*lt == L_CHAR) setbit(bm, *last);
    *lt = L_CLASS;
}

/* _M_expression_term: 0 at the closing ']' */
static int term(uint16_t bm, uint8_t *lt, uint8_t *last)
{
    rx_t *c = RX;
    uint16_t k, b;
    uint8_t ch;
    if (match_tok(T_BRACKET_END)) return 0;
    if (match_tok(T_COLLSYM)) {                     /* [.name.]: a character */
        k = lookup(COLLATE_NAMES, 0);
        if (k == NONE) rx_fail(RX_EINVAL);
        setbit(bm, (uint8_t)k);                     /* _M_add_collate_element adds it */
        push_char(bm, lt, last, (uint8_t)k);
    } else if (match_tok(T_EQUIV)) {                /* [=name=]: transform_primary is tolower */
        push_class(bm, lt, last);
        k = lookup(COLLATE_NAMES, 0);
        if (k == NONE) rx_fail(RX_EINVAL);
        ch = (uint8_t)k;
        if (ch >= 'A' && ch <= 'Z') ch += 32;
        setbit(bm, ch);
        if (ch >= 'a' && ch <= 'z') setbit(bm, (uint8_t)(ch - 32));
    } else if (match_tok(T_CLASS)) {                /* [:name:] */
        push_class(bm, lt, last);
        k = lookup(CLASS_NAMES, 1);
        if (k == NONE) rx_fail(RX_EINVAL);
        for (b = 0; b != 128; ++b) if (inclass(k, (uint8_t)b)) setbit(bm, (uint8_t)b);
    } else if (match_tok(T_ORD)) push_char(bm, lt, last, c->cvch);
    else if (match_tok(T_DASH)) {
        if (match_tok(T_BRACKET_END)) { push_char(bm, lt, last, '-'); return 0; }
        if (*lt != L_CHAR) rx_fail(RX_EINVAL);      /* after a class, or nothing: POSIX refuses */
        if (match_tok(T_ORD)) range(bm, *last, c->cvch);
        else if (match_tok(T_DASH)) range(bm, *last, '-');
        else rx_fail(RX_EINVAL);
        *lt = L_NONE;
    } else rx_fail(RX_EINVAL);
    return 1;
}

/* _M_insert_bracket_matcher: its SET state */
static uint16_t bracket(uint8_t neg)
{
    rx_t *c = RX;
    uint16_t bm, i;
    uint8_t lt = L_NONE, last = 0;
    if ((uint16_t)(c->hi - RX_STATES) < (uint16_t)((c->nst << 3) + 32)) rx_fail(RX_ESPACE);
    c->hi -= 32;
    bm = c->hi;
    memset(RX_P(bm), 0, 32);
    if (match_tok(T_ORD)) { lt = L_CHAR; last = c->cvch; }
    else if (match_tok(T_DASH)) { lt = L_CHAR; last = '-'; }
    while (term(bm, &lt, &last)) ;
    if (lt == L_CHAR) setbit(bm, last);
    if (neg) for (i = 0; i != 32; ++i) RXB(bm + i) ^= 0xFF;
    return insert(O_SET, NONE, bm);
}

/* ---- the compiler (regex_compiler.tcc) ---- */

/* _M_cur_int_value(10): an int, or error_backref past INT_MAX */
static uint32_t intval(void)
{
    rx_t *c = RX;
    uint32_t v = 0, t;
    uint16_t i;
    uint8_t d;
    for (i = 0; i != c->cvlen; ++i) {
        d = c->re[c->cvoff + i] - '0';
        if (v > 214748364UL || (v == 214748364UL && d > 7)) rx_fail(RX_EINVAL);
        t = v + v;
        v = t + t;
        v = v + v + t + d;
    }
    return v;
}

/* An interval, after its '{': _M_quantifier's last case */
static void interval(uint16_t *ts, uint16_t *te)
{
    uint16_t rs = *ts, re = *te, es, ee, a, b, end, s;
    uint32_t min, max, n = 0, i;
    int infi = 0;
    if (!match_tok(T_DUP)) rx_fail(RX_EINVAL);
    es = ee = insert(O_DUMMY, NONE, 0);
    min = intval();
    if (match_tok(T_COMMA)) {
        if (match_tok(T_DUP)) {
            max = intval();
            if (max < min) n = 0xFFFFFFFFUL;        /* __n < 0, refused below */
            else n = max - min;
        } else infi = 1;
    }
    if (!match_tok(T_INTERVAL_END)) rx_fail(RX_EINVAL);
    for (i = 0; i != min; ++i) {
        clone(rs, re, &a, &b);
        ST(ee)->next = a;
        ee = b;
    }
    if (infi) {
        clone(rs, re, &a, &b);
        s = insert(O_REPEAT, NONE, a);
        ST(b)->next = s;
        ST(ee)->next = s;
        ee = s;
    } else {
        if (n == 0xFFFFFFFFUL) rx_fail(RX_EINVAL);
        end = insert(O_DUMMY, NONE, 0);
        for (i = 0; i != n; ++i) {
            clone(rs, re, &a, &b);
            /* _M_insert_repeat(tmp.start, end), its next and alt swapped at the end */
            s = insert(O_REPEAT, end, a);
            ST(ee)->next = s;
            ee = b;
        }
        ST(ee)->next = end;
        ee = end;
    }
    *ts = es;
    *te = ee;
}

/* _M_quantifier, while there is one */
static void quantifiers(uint16_t *ts, uint16_t *te)
{
    uint16_t r, e;
    for (;;) {
        if (match_tok(T_STAR)) {
            r = insert(O_REPEAT, NONE, *ts);
            ST(*te)->next = r;
            *ts = *te = r;
        } else if (match_tok(T_PLUS)) {
            r = insert(O_REPEAT, NONE, *ts);
            ST(*te)->next = r;
            *te = r;
        } else if (match_tok(T_OPT)) {
            e = insert(O_DUMMY, NONE, 0);
            r = insert(O_REPEAT, NONE, *ts);
            ST(*te)->next = e;
            ST(r)->next = e;
            *ts = r;
            *te = e;
        } else if (match_tok(T_INTERVAL)) interval(ts, te);
        else return;
    }
}

/* _M_alternative's base case and _M_disjunction's step: the alternative
 * ends with a dummy, and joins the disjunction behind an ALT */
static void end_alternative(uint16_t f)
{
    uint16_t d = insert(O_DUMMY, NONE, 0), end, alt;
    if (FR_AS(f) == NONE) FR_AS(f) = d;
    else ST(FR_AE(f))->next = d;
    FR_AE(f) = d;
    if (FR_DS(f) == NONE) {
        FR_DS(f) = FR_AS(f);
        FR_DE(f) = d;
    } else {
        end = insert(O_DUMMY, NONE, 0);
        ST(FR_DE(f))->next = end;
        ST(d)->next = end;
        alt = insert(O_ALT, FR_AS(f), FR_DS(f));    /* next: the new arm; alt: the ones before */
        FR_DS(f) = alt;
        FR_DE(f) = end;
    }
    FR_AS(f) = NONE;
}

static void alt_append(uint16_t f, uint16_t ts, uint16_t te)
{
    if (FR_AS(f) == NONE) FR_AS(f) = ts;
    else ST(FR_AE(f))->next = ts;
    FR_AE(f) = te;
}

/* _Compiler's constructor. The grammar's recursion is through groups only,
 * and here it is a stack of their frames. */
void rx_compile(const char *re, uint16_t n)
{
    rx_t *c = RX;
    uint16_t f = RX_FRAMES, ts, te, a, g;
    uint32_t need;
    c->re = (const uint8_t *)re;
    c->rn = n;
    c->rp = 0;
    c->nst = 0;
    c->ngroups = 0;
    c->hi = RX_TOP;
    c->depth = 0;
    c->sstate = S_NORMAL;
    c->at_bstart = 0;
    c->vlen = 0;
    advance();
    FR_OPEN(f) = insert(O_SUB_BEGIN, NONE, c->ngroups++);    /* state 0, group 0 */
    FR_DS(f) = NONE;
    FR_AS(f) = NONE;
    for (;;) {
        if (match_tok(T_LINE_BEGIN)) { ts = insert(O_BOL, NONE, 0); alt_append(f, ts, ts); continue; }
        if (match_tok(T_LINE_END)) { ts = insert(O_EOL, NONE, 0); alt_append(f, ts, ts); continue; }
        if (match_tok(T_ANY)) ts = te = insert(O_ANY, NONE, 0);
        else if (match_tok(T_ORD)) ts = te = insert(O_CHAR, NONE, c->cvch);
        else if (match_tok(T_SUB_BEGIN)) {
            if (++c->depth == RX_DEPTH) rx_fail(RX_ESPACE);
            f += 16;
            FR_OPEN(f) = insert(O_SUB_BEGIN, NONE, c->ngroups++);
            FR_DS(f) = NONE;
            FR_AS(f) = NONE;
            continue;
        } else if (match_tok(T_BRACKET_NEG)) ts = te = bracket(1);
        else if (match_tok(T_BRACKET_BEGIN)) ts = te = bracket(0);
        else {
            /* no term: the alternative, and unless a '|' follows the disjunction, ends */
            end_alternative(f);
            if (match_tok(T_OR)) continue;
            if (c->depth == 0) break;
            if (!match_tok(T_SUB_END)) rx_fail(RX_EINVAL);
            ts = FR_OPEN(f);                        /* the group: begin, disjunction, end */
            ST(ts)->next = FR_DS(f);
            a = insert(O_SUB_END, NONE, ST(ts)->x);
            ST(FR_DE(f))->next = a;
            te = a;
            f -= 16;
            --c->depth;
        }
        quantifiers(&ts, &te);
        alt_append(f, ts, te);
    }
    if (!match_tok(T_EOF)) rx_fail(RX_EINVAL);      /* error_paren */
    ST(0)->next = FR_DS(f);
    a = insert(O_SUB_END, NONE, 0);
    ST(FR_DE(f))->next = a;
    ST(a)->next = insert(O_ACCEPT, NONE, 0);
    eliminate_dummy();
    /* the captures above the states: 5 bytes a group */
    g = c->ngroups;                                 /* at most a state each: 5g < 64 KB */
    c->cf = RX_STATES + (c->nst << 3);
    c->recsize = (g << 2) + g;
    need = (uint32_t)c->cf + c->recsize + c->recsize;   /* and room for one record */
    if (need > c->hi) rx_fail(RX_ESPACE);
    c->cs = c->cf + (g << 1);
    c->cm = c->cs + (g << 1);
    c->recs = c->cm + g;
}

uint16_t rx_groups(void) { return RX->ngroups; }

/* ---- the executor (regex_executor.tcc, __dfs_mode) ---- */

#define X_EXACT     1   /* _Match_mode::_Exact */
#define X_NOT_NULL  2   /* match_not_null */
#define X_PREV      4   /* match_prev_avail */
#define X_CONT      8   /* match_continuous */

/* The stack's records, newest lowest, each undoing one step of the search
 * when it backs out (the code after _M_dfs's recursive call). */
#define F_MATCH     1   /* [kind]                         --current */
#define F_SB        2   /* [kind][k][first]               group k's first back */
#define F_SE        3   /* [kind][k][second][matched]     group k's second and matched back */
#define F_REP       4   /* [kind][i][rpos][cnt]           state i's rep count back; then its exit */
#define F_REPDEC    5   /* [kind][i]                      state i's count less one; then its exit */
#define F_ALT       6   /* [kind][i]                      the ALT's other arm */
#define F_ALTT      7   /* [kind]                         has_sol |= true */
#define F_ALTF      8   /* [kind]                         has_sol |= false */

#define PUSH(n) do { if ((uint16_t)(sp - lo) < (n)) rx_fail(RX_ESPACE); sp -= (n); p = RX_P(sp); } while (0)

/* _M_main_dispatch from begin: 1 if a solution, its groups in the record
 * at c->best. POSIX keeps the first of the longest (_M_handle_accept). */
static int exec(uint16_t begin, uint8_t fl)
{
    rx_t *c = RX;
    const uint8_t *s = c->s;
    uint16_t end = c->sn, cur = begin, sol = NONE, i = 0, k;
    uint16_t base = c->hi, sp = c->hi, lo = c->lo, cf = c->cf, cs = c->cs, cm = c->cm;
    uint8_t has = 0, b;
    rx_state *st;
    uint8_t *p;

    memset(RX_P(cm), 0, c->ngroups);                /* _M_cur_results: nothing matched */
descend:
    st = ST(i);
    switch (st->op) {
    case O_REPEAT:                                  /* _M_rep_once_more, then the exit */
        if (st->cnt == 0 || st->rpos != cur) {
            PUSH(6);
            p[0] = F_REP;
            W(p + 1) = i;
            W(p + 3) = st->rpos;
            p[5] = st->cnt;
            st->rpos = cur;
            st->cnt = 1;
            i = st->x;
            goto descend;
        }
        if (st->cnt < 2) {
            PUSH(3);
            p[0] = F_REPDEC;
            W(p + 1) = i;
            ++st->cnt;
            i = st->x;
            goto descend;
        }
        i = st->next;                               /* (nothing has matched: has is 0 here) */
        goto descend;
    case O_SUB_BEGIN:
        k = st->x;
        PUSH(5);
        p[0] = F_SB;
        W(p + 1) = k;
        W(p + 3) = RXW(cf + (k << 1));
        RXW(cf + (k << 1)) = cur;
        i = st->next;
        goto descend;
    case O_SUB_END:
        k = st->x;
        PUSH(6);
        p[0] = F_SE;
        W(p + 1) = k;
        W(p + 3) = RXW(cs + (k << 1));
        p[5] = RXB(cm + k);
        RXW(cs + (k << 1)) = cur;
        RXB(cm + k) = 1;
        i = st->next;
        goto descend;
    case O_BOL:                                     /* _M_at_begin, not multiline */
        if (cur == begin && !(fl & X_PREV)) { i = st->next; goto descend; }
        break;
    case O_EOL:
        if (cur == end) { i = st->next; goto descend; }
        break;
    case O_ANY:                                     /* POSIX: anything but a NUL */
        if (cur != end && s[cur] != 0) goto step;
        break;
    case O_CHAR:
        if (cur != end && s[cur] == (uint8_t)st->x) goto step;
        break;
    case O_SET:
        if (cur != end) {
            b = s[cur];
            if (RXB(st->x + (b >> 3)) & BIT(b)) goto step;
        }
        break;
    case O_ACCEPT:
        has = (fl & X_EXACT) ? cur == end : 1;
        if (cur == begin && (fl & X_NOT_NULL)) has = 0;
        if (has && (sol == NONE || sol < cur)) {
            sol = cur;
            memcpy(RX_P(c->best), RX_P(cf), c->recsize);
        }
        break;
    case O_ALT:
        PUSH(3);
        p[0] = F_ALT;
        W(p + 1) = i;
        i = st->x;
        goto descend;
    default:                                        /* a DUMMY: none is reachable */
        break;
    }
    /* back out, undoing, until a record resumes the search */
    for (;;) {
        if (sp == base) return has;
        p = RX_P(sp);
        switch (p[0]) {
        case F_MATCH:
            --cur;
            ++sp;
            break;
        case F_SB:
            RXW(cf + (W(p + 1) << 1)) = W(p + 3);
            sp += 5;
            break;
        case F_SE:
            k = W(p + 1);
            RXW(cs + (k << 1)) = W(p + 3);
            RXB(cm + k) = p[5];
            sp += 6;
            break;
        case F_REP:
            i = W(p + 1);
            st = ST(i);
            st->rpos = W(p + 3);
            st->cnt = p[5];
            sp += 6;
            if (!has) { i = st->next; goto descend; }
            break;
        case F_REPDEC:
            i = W(p + 1);
            st = ST(i);
            --st->cnt;
            sp += 3;
            if (!has) { i = st->next; goto descend; }
            break;
        case F_ALT:                                 /* _M_handle_alternative, POSIX */
            i = W(p + 1);
            sp += 2;
            p = RX_P(sp);
            p[0] = has ? F_ALTT : F_ALTF;
            has = 0;
            i = ST(i)->next;
            goto descend;
        case F_ALTT:
            has = 1;
            ++sp;
            break;
        default:                                    /* F_ALTF */
            ++sp;
            break;
        }
    }
step:                                               /* _M_handle_match's ++current */
    PUSH(1);
    p[0] = F_MATCH;
    ++cur;
    i = st->next;
    goto descend;
}

/* _M_search: from start, then (unless continuous) each later position */
static int search(uint16_t start, uint8_t fl)
{
    uint16_t b = start, n = RX->sn;
    if (exec(b, fl)) return 1;
    if (fl & X_CONT) return 0;
    fl |= X_PREV;
    while (b != n) {
        ++b;
        if (exec(b, fl)) return 1;
    }
    return 0;
}

static void subject(const char *s, uint16_t n)
{
    rx_t *c = RX;
    c->s = (const uint8_t *)s;
    c->sn = n;
}

/* The record the executor writes next, and its floor above it */
static void slot(uint16_t rec)
{
    rx_t *c = RX;
    if ((uint32_t)rec + c->recsize > c->hi) rx_fail(RX_ESPACE);
    c->best = rec;
    c->lo = rec + c->recsize;
}

int rx_match(const char *s, uint16_t n)
{
    subject(s, n);
    slot(RX->recs);
    return exec(0, X_EXACT);
}

/* regex_iterator's constructor and operator++: after an empty match, the
 * same place again but not empty (and without match_prev_avail, until the
 * first time that fails), then one on; match_prev_avail from then on. */
uint16_t rx_split(const char *s, uint16_t n)
{
    rx_t *c = RX;
    uint16_t cnt = 0, rec = c->recs, start, f0;
    uint8_t fl = 0;
    subject(s, n);
    slot(rec);
    if (!search(0, fl)) return 0;
    for (;;) {
        ++cnt;
        f0 = rx_first(rec, 0);
        start = rx_second(rec, 0);
        rec += c->recsize;
        slot(rec);
        if (f0 == start) {
            if (start == n) break;
            if (search(start, fl | X_NOT_NULL | X_CONT)) continue;
            ++start;
        }
        fl |= X_PREV;
        if (!search(start, fl)) break;
    }
    return cnt;
}

uint16_t rx_records(void) { return RX->recs; }
uint16_t rx_recsize(void) { return RX->recsize; }
uint16_t rx_first(uint16_t off, uint16_t k) { return RXW(off + (k << 1)); }
uint16_t rx_second(uint16_t off, uint16_t k) { return RXW(off + (RX->ngroups << 1) + (k << 1)); }
uint8_t  rx_matched(uint16_t off, uint16_t k) { return RXB(off + (RX->ngroups << 2) + k); }
