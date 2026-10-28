/* parse.c -- rnix 0.14.0's parser, building rnix's syntax tree (docs/
 * COMPILER.md "Stage 2"): the same nodes, tokens and trivia, in the same
 * places, so that the lowering (stage 2b) reads the tree nbc's front.rs
 * reads. Each function is the one of the same name in parser.rs, its
 * control flow that one's.
 *
 * One difference, by design: rnix recovers from an error and goes on, and
 * nbc refuses any input it reports an error for, so the tree after an error
 * never matters. Here the first error is E_PARSE. Up to it the two parsers
 * take the same steps, so they agree on whether there is one at all.
 *
 * The builder is rowan's GreenNodeBuilder: a stack of finished children
 * (tokens and nodes) and a stack of open parents, each the index of its
 * first child; a checkpoint is the children's count, start_node_at opens a
 * parent at one, and finish_node wraps everything since its first child
 * into a node. Finished children move to the store, in order.
 */
#pragma noroot
#pragma optimize 15

#include "vm.h"
#include "front.h"

segment "front";

struct tk { uint8_t kind, pad; uint16_t start, len; };
struct par { uint8_t kind, pad; uint16_t first; };

#define BUF      ((struct tk *)(FRONT_BASE + 0x4000UL))    /* rnix's `buffer`: raw tokens, a ring */
#define TRIV     ((struct tk *)(FRONT_BASE + 0x4C00UL))    /* `trivia_buffer`: trivia not yet given to the builder */
#define PARS     ((struct par *)(FRONT_BASE + 0x5800UL))   /* the builder's open parents */
#define WORK     ((struct el *)(FRONT_BASE + 0x6800UL))    /* the builder's children, to $0EFFFF */
#define STORE    CST_STORE                                  /* finished nodes' children: bank 15 */
#define RING     512U
#define PARS_N   1024U
#define WORK_N   6485U                                      /* ($10000 - $6800) / 6, rounded down */
#define STORE_N  10922U                                     /* $10000 / 6 */

static uint16_t bhead, bn, tn, pn, wn, sn, consumed, depth, root_w;

_Noreturn static void parse_err(void) { err_exit(E_PARSE); }

static int trivia(uint8_t k) { return k == TK_COMMENT || k == TK_WHITESPACE; }

/* The token sets the grammar asks about. A function and not tables: a static
 * array would be bank 0's (every static is), and this is code in "front". */
enum { S_IDENT, S_STRING_PARTS, S_ATTR_NAMES, S_PATTERN_NEXT, S_CONCAT, S_MUL, S_ADD, S_MERGE,
       S_COMPARE, S_EQUAL, S_AND, S_OR, S_IMPL, S_PIPE_RIGHT, S_PIPE_LEFT };

static int in_set(uint8_t set, uint8_t k)
{
    switch (set) {
    case S_IDENT:        return k == TK_IDENT;
    case S_STRING_PARTS: return k == TK_STRING_END || k == TK_STRING_CONTENT || k == TK_INTERPOL_START;
    case S_ATTR_NAMES:   return k == TK_IDENT || k == TK_OR || k == TK_CUR_POS;
    case S_PATTERN_NEXT: return k == TK_R_BRACE || k == TK_ELLIPSIS || k == TK_IDENT;
    case S_CONCAT:       return k == TK_CONCAT;
    case S_MUL:          return k == TK_MUL || k == TK_DIV;
    case S_ADD:          return k == TK_ADD || k == TK_SUB;
    case S_MERGE:        return k == TK_UPDATE;
    case S_COMPARE:      return k == TK_LESS || k == TK_LESS_OR_EQ || k == TK_MORE || k == TK_MORE_OR_EQ;
    case S_EQUAL:        return k == TK_EQUAL || k == TK_NOT_EQUAL;
    case S_AND:          return k == TK_AND_AND;
    case S_OR:           return k == TK_OR_OR;
    case S_IMPL:         return k == TK_IMPLICATION;
    case S_PIPE_RIGHT:   return k == TK_PIPE_RIGHT;
    case S_PIPE_LEFT:    return k == TK_PIPE_LEFT;
    }
    return 0;
}

/* --- the builder (rowan) ---------------------------------------------------------- */
static void b_token(uint8_t kind, uint16_t start, uint16_t len)
{
    if (wn >= WORK_N) nomem();
    WORK[wn].kind = kind;
    WORK[wn].a = start;
    WORK[wn].b = len;
    ++wn;
}

static void b_open(uint8_t kind, uint16_t first)
{
    if (pn >= PARS_N) nomem();
    PARS[pn].kind = kind;
    PARS[pn].first = first;
    ++pn;
}

static void finish_node(void)
{
    uint16_t first, n, i;
    --pn;
    first = PARS[pn].first;
    n = (uint16_t)(wn - first);
    if ((uint16_t)(STORE_N - sn) < n) nomem();
    for (i = 0; i < n; ++i) {                       /* fields: a struct copy would link ~MOVE */
        STORE[sn + i].kind = WORK[first + i].kind;
        STORE[sn + i].a = WORK[first + i].a;
        STORE[sn + i].b = WORK[first + i].b;
    }
    WORK[first].kind = PARS[pn].kind;
    WORK[first].a = sn;
    WORK[first].b = n;
    sn += n;
    wn = (uint16_t)(first + 1);
}

/* --- tokens (rnix's buffer, trivia_buffer, iter) -------------------------------------- */
/* Pull one token from the lexer onto the back of the buffer: 0 at the end. */
static int pull(void)
{
    uint16_t s;
    uint8_t k = lex_next(&s);
    struct tk *t;
    if (k == TK_EOF) return 0;
    if (bn >= RING) nomem();
    t = &BUF[(bhead + bn) & (RING - 1)];
    t->kind = k;
    t->start = s;
    t->len = (uint16_t)(lex_off - s);
    ++bn;
    return 1;
}

#define BUFAT(i) BUF[(bhead + (i)) & (RING - 1)]

static int try_next(struct tk *t)
{
    uint16_t s;
    if (bn) {
        t->kind = BUF[bhead].kind;
        t->start = BUF[bhead].start;
        t->len = BUF[bhead].len;
        bhead = (bhead + 1) & (RING - 1);
        --bn;
        return 1;
    }
    t->kind = lex_next(&s);
    if (t->kind == TK_EOF) return 0;
    t->start = s;
    t->len = (uint16_t)(lex_off - s);
    return 1;
}

/* The front raw token's kind, TK_EOF at the end. */
static uint8_t peek_raw(void)
{
    if (!bn && !pull()) return TK_EOF;
    return BUF[bhead].kind;
}

static void drain_trivia(void)
{
    uint16_t i;
    for (i = 0; i < tn; ++i) {
        consumed += TRIV[i].len;
        b_token(TRIV[i].kind, TRIV[i].start, TRIV[i].len);
    }
    tn = 0;
}

static void manual_bump(const struct tk *t, uint8_t kind)
{
    consumed += t->len;
    b_token(kind, t->start, t->len);
}

static void bump(void)
{
    struct tk t;
    if (!try_next(&t)) parse_err();                 /* UnexpectedEOF */
    if (trivia(t.kind)) {
        if (tn >= RING) nomem();
        TRIV[tn].kind = t.kind;
        TRIV[tn].start = t.start;
        TRIV[tn].len = t.len;
        ++tn;
        return;
    }
    drain_trivia();
    manual_bump(&t, t.kind);
}

static uint8_t peek_ahead(uint16_t n)
{
    uint16_t seen = 0, cur = 0;
    uint8_t k;
    for (;;) {
        if (bn <= cur && !pull()) return TK_EOF;
        k = BUFAT(cur).kind;
        ++cur;
        if (!trivia(k)) {
            if (seen == n) return k;
            ++seen;
        }
    }
}

static uint8_t tpeek(void)                         /* rnix's peek(); vm.h has a peek() of its own */
{
    while (trivia(peek_raw())) bump();
    return peek_ahead(0);
}

static void eat_trivia(void)
{
    tpeek();
    drain_trivia();
}

static void start_node(uint8_t kind)
{
    eat_trivia();
    b_open(kind, wn);
}

static uint16_t checkpoint(void)
{
    eat_trivia();
    return wn;
}

#define start_node_at(cp, kind) b_open((kind), (cp))

/* expect_peek_any: the next token if it is one of `allowed` (TK_EOF-ended);
 * anything else, or the end, is the error rnix reports. */
static uint8_t expect_peek_any(uint8_t set)
{
    uint8_t k = tpeek();
    if (k == TK_EOF) parse_err();                   /* UnexpectedEOFWanted */
    if (!in_set(set, k)) parse_err();               /* UnexpectedWanted */
    return k;
}

static void expect(uint8_t k)
{
    uint8_t got = tpeek();
    if (got == TK_EOF || got != k) parse_err();     /* expect_peek_any(&[k]) */
    bump();
}

static void expect_ident(void)
{
    expect_peek_any(S_IDENT);
    start_node(NODE_IDENT);
    bump();
    finish_node();
}

/* --- the grammar ------------------------------------------------------------------- */
static uint16_t parse_expr(void);
static uint16_t parse_simple(void);

static void parse_dynamic(void)
{
    uint8_t k;
    start_node(NODE_DYNAMIC);
    bump();
    while ((k = tpeek()) != TK_EOF && k != TK_INTERPOL_END) parse_expr();
    bump();
    finish_node();
}

static void parse_string(void)
{
    uint8_t k;
    start_node(NODE_STRING);
    expect(TK_STRING_START);
    for (;;) {
        k = expect_peek_any(S_STRING_PARTS);
        if (k == TK_STRING_CONTENT) {
            bump();
        } else if (k == TK_INTERPOL_START) {
            start_node(NODE_INTERPOL);
            bump();
            parse_expr();
            expect(TK_INTERPOL_END);
            finish_node();
        } else {
            break;
        }
    }
    expect(TK_STRING_END);
    finish_node();
}

static void parse_attr(void)
{
    struct tk t;
    uint8_t k = tpeek();
    if (k == TK_INTERPOL_START) {
        parse_dynamic();
    } else if (k == TK_STRING_START) {
        parse_string();
    } else {
        expect_peek_any(S_ATTR_NAMES);
        start_node(NODE_IDENT);
        try_next(&t);
        manual_bump(&t, TK_IDENT);                  /* `or` and `__curPos` too: an IDENT here */
        finish_node();
    }
}

static void parse_attrpath(void)
{
    start_node(NODE_ATTRPATH);
    for (;;) {
        parse_attr();
        if (tpeek() == TK_DOT) bump();
        else break;
    }
    finish_node();
}

static void parse_pattern(int bound)
{
    uint8_t k = tpeek();
    if (k == TK_R_BRACE || k == TK_EOF) {
        bump();
    } else {
        for (;;) {
            k = expect_peek_any(S_PATTERN_NEXT);
            if (k == TK_R_BRACE) {
                bump();
                break;
            }
            if (k == TK_ELLIPSIS) {
                bump();
                expect(TK_R_BRACE);
                break;
            }
            start_node(NODE_PAT_ENTRY);
            expect_ident();
            if (tpeek() == TK_QUESTION) {
                bump();
                parse_expr();
            }
            finish_node();
            if (tpeek() == TK_COMMA) {
                bump();
            } else {
                expect(TK_R_BRACE);
                break;
            }
        }
    }
    if (tpeek() == TK_AT) {
        if (bound) parse_err();                     /* UnexpectedDoubleBind */
        start_node(NODE_PAT_BIND);
        bump();
        expect_ident();
        finish_node();
    }
}

static void parse_set(uint8_t until)
{
    uint8_t k;
    for (;;) {
        k = tpeek();
        if (k == TK_EOF || k == until) break;
        if (k == TK_INHERIT) {
            start_node(NODE_INHERIT);
            bump();
            if (tpeek() == TK_L_PAREN) {
                start_node(NODE_INHERIT_FROM);
                bump();
                parse_expr();
                expect(TK_R_PAREN);
                finish_node();
            }
            for (;;) {
                k = tpeek();
                if (k == TK_EOF) parse_err();       /* UnexpectedEOF */
                if (k == TK_SEMICOLON) break;
                parse_attr();
            }
            expect(TK_SEMICOLON);
            finish_node();
        } else {
            start_node(NODE_ATTRPATH_VALUE);
            parse_attrpath();
            expect(TK_ASSIGN);
            parse_expr();
            expect(TK_SEMICOLON);
            finish_node();
        }
    }
    bump();                                         /* the final close, like '}' */
}

static int is_literal(uint8_t k)
{
    return k == TK_FLOAT || k == TK_INTEGER || k == TK_PATH_ABS || k == TK_PATH_REL ||
           k == TK_PATH_HOME || k == TK_PATH_SEARCH || k == TK_URI;
}

static int is_fn_arg(uint8_t k)
{
    return k == TK_REC || k == TK_L_BRACE || k == TK_L_BRACK || k == TK_L_PAREN ||
           k == TK_STRING_START || k == TK_IDENT || k == TK_CUR_POS || is_literal(k);
}

/* Whether the front raw token's text is `__curPos` */
static int is_cur_pos(void)
{
    const char *w = "__curPos";                     /* a literal stays with its code */
    const struct tk *t = &BUF[bhead];
    const uint8_t *s = lex_text();                  /* an import's, not SRC_BASE's */
    uint16_t i;
    if (t->len != 8) return 0;
    for (i = 0; i < 8; ++i)
        if (s[t->start + i] != (uint8_t)w[i]) return 0;
    return 1;
}

/* The `{` lookahead: two non-trivia kinds, read straight from the lexer and
 * appended to the buffer, as rnix does -- after whatever the buffer holds. */
static uint8_t lookahead(void)
{
    uint16_t s;
    uint8_t k;
    struct tk *t;
    for (;;) {
        k = lex_next(&s);
        if (k == TK_EOF) return TK_EOF;
        if (bn >= RING) nomem();
        t = &BUF[(bhead + bn) & (RING - 1)];
        t->kind = k;
        t->start = s;
        t->len = (uint16_t)(lex_off - s);
        ++bn;
        if (!trivia(k)) return k;
    }
}

static uint16_t parse_simple(void)
{
    uint8_t k = tpeek(), p0, p1, node;
    uint16_t cp;
    struct tk t;
    if (k == TK_EOF) parse_err();                   /* UnexpectedEOF */
    cp = checkpoint();
    switch (k) {
    case TK_L_PAREN:
        start_node(NODE_PAREN);
        bump();
        parse_expr();
        bump();
        finish_node();
        break;
    case TK_REC:
        start_node(NODE_ATTR_SET);
        bump();
        expect(TK_L_BRACE);
        parse_set(TK_R_BRACE);
        finish_node();
        break;
    case TK_L_BRACE:
        p0 = lookahead();
        p1 = lookahead();
        if ((p0 == TK_IDENT && (p1 == TK_COMMA || p1 == TK_QUESTION || p1 == TK_R_BRACE)) ||
            (p0 == TK_ELLIPSIS && p1 == TK_R_BRACE) ||
            (p0 == TK_R_BRACE && (p1 == TK_COLON || p1 == TK_AT))) {
            start_node(NODE_LAMBDA);                /* this looks like a pattern */
            start_node(NODE_PATTERN);
            bump();
            parse_pattern(0);
            finish_node();
            expect(TK_COLON);
            parse_expr();
            finish_node();
        } else {
            start_node(NODE_ATTR_SET);              /* this looks like a set */
            bump();
            parse_set(TK_R_BRACE);
            finish_node();
        }
        break;
    case TK_L_BRACK:
        start_node(NODE_LIST);
        bump();
        while ((k = tpeek()) != TK_EOF && k != TK_R_BRACK) parse_simple();
        bump();
        finish_node();
        break;
    case TK_STRING_START:
        parse_string();
        break;
    case TK_PATH_ABS: case TK_PATH_REL: case TK_PATH_HOME: case TK_PATH_SEARCH:
        node = k == TK_PATH_ABS ? NODE_PATH_ABS : k == TK_PATH_REL ? NODE_PATH_REL :
               k == TK_PATH_HOME ? NODE_PATH_HOME : NODE_PATH_SEARCH;
        start_node(node);
        bump();
        /* search paths take no interpolation; tpeek() skips trivia, the loop does not */
        if (node != NODE_PATH_SEARCH && tpeek() == TK_INTERPOL_START) {
            for (;;) {
                k = peek_raw();
                if (k == TK_PATH_ABS || k == TK_PATH_REL || k == TK_PATH_HOME) {
                    bump();
                } else if (k == TK_INTERPOL_START) {
                    start_node(NODE_INTERPOL);
                    bump();
                    parse_expr();
                    expect(TK_INTERPOL_END);
                    finish_node();
                } else {
                    break;
                }
            }
        }
        finish_node();
        break;
    case TK_IDENT:
        k = peek_ahead(1);
        if (k == TK_COLON) {
            expect_ident();
            start_node_at(cp, NODE_LAMBDA);
            start_node_at(cp, NODE_IDENT_PARAM);
            finish_node();
            expect(TK_COLON);
            parse_expr();
            finish_node();
        } else if (k == TK_AT) {
            expect_ident();
            start_node_at(cp, NODE_LAMBDA);
            start_node_at(cp, NODE_PATTERN);
            start_node_at(cp, NODE_PAT_BIND);
            expect(TK_AT);
            finish_node();                          /* PatBind */
            expect(TK_L_BRACE);
            parse_pattern(1);
            finish_node();                          /* Pattern */
            expect(TK_COLON);
            parse_expr();
            finish_node();                          /* Lambda */
        } else if (is_cur_pos()) {
            start_node(NODE_CUR_POS);
            try_next(&t);
            manual_bump(&t, TK_CUR_POS);
            finish_node();
        } else {
            expect_ident();
        }
        break;
    default:
        if (!is_literal(k)) parse_err();            /* UnexpectedWanted */
        start_node(NODE_LITERAL);
        bump();
        finish_node();
    }

    k = tpeek();
    if (k == TK_DOT) {
        start_node_at(cp, NODE_SELECT);
        bump();
        parse_attrpath();
        if (tpeek() == TK_OR) {
            bump();
            parse_simple();
        }
        finish_node();
    } else if (k == TK_OR) {
        /* "a b or c" => "((a (b or)) c)", as Nix's parser has it */
        start_node_at(cp, NODE_APPLY);
        start_node(NODE_IDENT);
        try_next(&t);
        manual_bump(&t, TK_IDENT);
        finish_node();
        finish_node();
    }
    return cp;
}

static uint16_t parse_fn(void)
{
    uint16_t cp = parse_simple();
    while (is_fn_arg(tpeek())) {
        start_node_at(cp, NODE_APPLY);
        parse_simple();
        finish_node();
    }
    return cp;
}

static uint16_t parse_negate(void)
{
    uint16_t cp;
    if (tpeek() != TK_SUB) return parse_fn();
    cp = checkpoint();
    start_node(NODE_UNARY_OP);
    bump();
    parse_negate();
    finish_node();
    return cp;
}

typedef uint16_t (*level_fn)(void);

static uint16_t parse_non_assoc(level_fn next, uint8_t ops)
{
    uint16_t cp = next();
    if (in_set(ops, tpeek())) {
        start_node_at(cp, NODE_BIN_OP);
        bump();
        next();
        finish_node();
    }
    return cp;
}

static uint16_t parse_left_assoc(level_fn next, uint8_t ops)
{
    uint16_t cp = next();
    while (in_set(ops, tpeek())) {
        start_node_at(cp, NODE_BIN_OP);
        bump();
        next();
        finish_node();
    }
    return cp;
}

static uint16_t parse_right_assoc(level_fn next, uint8_t ops)
{
    uint16_t cp = next();
    if (in_set(ops, tpeek())) {
        start_node_at(cp, NODE_BIN_OP);
        bump();
        parse_right_assoc(next, ops);
        finish_node();
    }
    return cp;
}

static uint16_t parse_hasattr(void)
{
    uint16_t cp = parse_negate();
    while (tpeek() == TK_QUESTION) {
        start_node_at(cp, NODE_HAS_ATTR);
        bump();
        parse_attrpath();
        finish_node();
    }
    return cp;
}

static uint16_t parse_concat(void) { return parse_right_assoc(parse_hasattr, S_CONCAT); }
static uint16_t parse_mul(void) { return parse_left_assoc(parse_concat, S_MUL); }
static uint16_t parse_add(void) { return parse_left_assoc(parse_mul, S_ADD); }

static uint16_t parse_invert(void)
{
    uint16_t cp;
    if (tpeek() != TK_INVERT) return parse_add();
    cp = checkpoint();
    start_node(NODE_UNARY_OP);
    bump();
    parse_invert();
    finish_node();
    return cp;
}

static uint16_t parse_merge(void) { return parse_right_assoc(parse_invert, S_MERGE); }
static uint16_t parse_compare(void) { return parse_non_assoc(parse_merge, S_COMPARE); }
static uint16_t parse_equal(void) { return parse_non_assoc(parse_compare, S_EQUAL); }
static uint16_t parse_and(void) { return parse_left_assoc(parse_equal, S_AND); }
static uint16_t parse_or(void) { return parse_left_assoc(parse_and, S_OR); }
static uint16_t parse_implication(void) { return parse_right_assoc(parse_or, S_IMPL); }
static uint16_t parse_pipe_right(void) { return parse_left_assoc(parse_implication, S_PIPE_RIGHT); }
static uint16_t parse_pipe_left(void) { return parse_right_assoc(parse_pipe_right, S_PIPE_LEFT); }

static uint16_t parse_expr(void)
{
    uint16_t cp;
    stack_check();                                  /* nesting is recursion: E_NOMEM, never an overflow */
    if (depth >= 512) parse_err();                  /* RecursionLimitExceeded */
    ++depth;
    switch (tpeek()) {
    case TK_LET:
        cp = checkpoint();
        bump();
        if (tpeek() == TK_L_BRACE) {
            start_node_at(cp, NODE_LEGACY_LET);
            bump();
            parse_set(TK_R_BRACE);
            finish_node();
        } else {
            start_node_at(cp, NODE_LET_IN);
            parse_set(TK_IN);
            parse_expr();
            finish_node();
        }
        break;
    case TK_WITH:
        cp = checkpoint();
        start_node(NODE_WITH);
        bump();
        parse_expr();
        expect(TK_SEMICOLON);
        parse_expr();
        finish_node();
        break;
    case TK_IF:
        cp = checkpoint();
        start_node(NODE_IF_ELSE);
        bump();
        parse_expr();
        expect(TK_THEN);
        parse_expr();
        expect(TK_ELSE);
        parse_expr();
        finish_node();
        break;
    case TK_ASSERT:
        cp = checkpoint();
        start_node(NODE_ASSERT);
        bump();
        parse_expr();
        expect(TK_SEMICOLON);
        parse_expr();
        finish_node();
        break;
    default:
        cp = parse_pipe_left();
    }
    --depth;
    return cp;
}

/* --- the whole file ------------------------------------------------------------------ */
void parse_file(const uint8_t *text, uint16_t n)
{
    wn = sn = 0;
    parse_file_at(text, n);
}

/* Another file's tree, after the trees already built -- an import's, while
 * its importer's is still being lowered: cst_mark first, cst_release after. */
void parse_file_at(const uint8_t *text, uint16_t n)
{
    lex_init(text, n);
    bhead = bn = tn = pn = consumed = depth = 0;
    root_w = wn;
    b_open(NODE_ROOT, wn);                          /* not start_node: the leading trivia are the root's */
    parse_expr();
    eat_trivia();
    if (tpeek() != TK_EOF) parse_err();              /* UnexpectedExtra */
    finish_node();
}

void front_run(void)
{
    switch (*(volatile uint8_t *)FRONT_MODE_ADDR) {
    case FRONT_LEX: lex_dump(); break;
    case FRONT_CST: cst_dump(); break;
    case FRONT_AST: ast_dump(); break;
    case FRONT_COMPILE: compile_dump(); break;
    default: err_exit(E_BADNBC);
    }
}

/* The last tree's root: WORK[root_w]. Its children are the store's. */
const struct el *cst_root(void) { return &WORK[root_w]; }

void cst_mark(struct cst_mark *m) { m->root = root_w; m->w = wn; m->s = sn; }
void cst_release(const struct cst_mark *m) { root_w = m->root; wn = m->w; sn = m->s; }
const struct el *cst_child(const struct el *node, uint16_t i) { return &STORE[node->a + i]; }

/* --cst (a2min): the tree in preorder, as `nbc --cst` prints rnix's: a
 * record a node or token -- depth, kind, start, length -- in the image
 * bank (unused: no image runs), their count at RESULT_ADDR. Walked with a
 * stack of its own, in the builder's parents' place: a parse's nesting is
 * recursion, a dump's need not be. */
struct walk { uint16_t el, next, rec; };
#define WALK  ((struct walk *)(FRONT_BASE + 0x4000UL))   /* the token rings', idle now: 1,024 */
#define OUT   ((uint8_t *)IMAGE_BASE)

static uint16_t rec_out(uint16_t count, uint16_t depth_, uint8_t kind, uint16_t start, uint16_t len)
{
    uint8_t *r;
    if (count >= 0x2492U) nomem();                  /* seven bytes a record, one bank */
    r = OUT + 7U * count;
    r[0] = (uint8_t)depth_; r[1] = (uint8_t)(depth_ >> 8);
    r[2] = kind;
    r[3] = (uint8_t)start; r[4] = (uint8_t)(start >> 8);
    r[5] = (uint8_t)len; r[6] = (uint8_t)(len >> 8);
    return count;
}

void cst_dump(void)
{
    uint16_t n = *(volatile uint16_t *)SRC_LEN_ADDR, sp = 0, count = 0, at = 0, i, len;
    const struct el *e;
    uint8_t *r;
    parse_file((const uint8_t *)SRC_BASE, n);
    WALK[0].el = 0xFFFFU;                           /* the root, WORK[0] */
    WALK[0].next = 0;
    WALK[0].rec = rec_out(count++, 0, NODE_ROOT, 0, 0);
    sp = 1;
    while (sp) {
        struct walk *w = &WALK[sp - 1];
        const struct el *node = w->el == 0xFFFFU ? &WORK[0] : &STORE[w->el];
        if (w->next == node->b) {                   /* done: its length, now that it is known */
            r = OUT + 7U * w->rec;
            len = (uint16_t)(at - (r[3] | ((uint16_t)r[4] << 8)));
            r[5] = (uint8_t)len; r[6] = (uint8_t)(len >> 8);
            --sp;
            continue;
        }
        i = (uint16_t)(node->a + w->next++);
        e = &STORE[i];
        if (e->kind >= NODE_FIRST) {
            if (sp >= 1024U) nomem();
            WALK[sp].el = i;
            WALK[sp].next = 0;
            WALK[sp].rec = rec_out(count++, sp, e->kind, at, 0);
            ++sp;
        } else {
            rec_out(count++, sp, e->kind, e->a, e->b);
            at = (uint16_t)(e->a + e->b);
        }
    }
    *(uint16_t *)RESULT_ADDR = count;
}
