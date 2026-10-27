/* toml.c -- TOML 1.0 as Nix 2.34's builtins.fromTOML parses it (docs/TOML.md).
 *
 * Nix parses with toml11 4.4.0 -- toml::parse of the string, spec v1.0.0,
 * no extensions -- and its answers, which documents are TOML and what they
 * mean, are toml11's. So this is a port of toml11's parser.hpp and
 * syntax_impl.hpp, procedure for procedure: the grammar's scanners as PEG
 * functions (sequence all or nothing, either the first that matches, repeat
 * greedy, never backtracking into an alternative), parse_file, parse_table,
 * parse_array, parse_inline_table and insert_value's rules for which table
 * may be reopened and how. A date or a time is an error, as it is in Nix
 * without the parse-toml-timestamps feature: any such value scans as an
 * integer followed by '-' or ':', an error in toml11's guess too.
 *
 * Derived from toml11 (https://github.com/ToruNiina/toml11):
 *   Copyright (c) 2017-now Toru Niina. MIT License:
 *   Permission is hereby granted, free of charge, to any person obtaining a
 *   copy of this software and associated documentation files (the
 *   "Software"), to deal in the Software without restriction, including
 *   without limitation the rights to use, copy, modify, merge, publish,
 *   distribute, sublicense, and/or sell copies of the Software, and to
 *   permit persons to whom the Software is furnished to do so, subject to
 *   the following conditions: The above copyright notice and this
 *   permission notice shall be included in all copies or substantial
 *   portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT
 *   WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
 *   THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 *   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 *   LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 *   OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 *   WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Nothing here allocates or runs bytecode. Bank 7 (TOML_BASE, the regex
 * engine's too) holds this context, a copy of the document with toml11's
 * newline appended, and an arena of nodes after it. Positions are 16-bit
 * offsets into the copy. An error is tm_fail, the caller's.
 */
#pragma noroot
#pragma optimize 15

#include <string.h>
#include "toml.h"

/* A segment of its own (VM.md). */
segment "toml";

#ifdef TM_HOST
extern uint8_t *tm_mem;                 /* 64 KB of the host's, for testing it there */
#define TM_P(off) (tm_mem + (uint16_t)(off))
#else
#define TM_P(off) ((uint8_t *)(TOML_BASE | (uint32_t)(uint16_t)(off)))
#endif
#define TMW(off)  (*(uint16_t *)TM_P(off))
#define TMB(off)  (*TM_P(off))

#define NONE      0xFFFFU

uint64_t f_strtod(const char *s, uint16_t n);   /* float.c: strtod, exactly */

typedef struct {
    uint16_t n;             /* the document's length, the newline appended */
    uint16_t top;           /* the arena's first free byte */
    uint16_t depth;         /* arrays and inline tables open */
    uint16_t val;           /* the node a parse made */
    uint8_t  nl;            /* skip_multiline_spacer saw a newline */
    uint8_t  has;           /* parse_comment_line found a comment */
} tm_t;
#define TM        ((tm_t *)TM_P(0))
#define TM_IN     0x0010            /* the document */
#define IN(p)     TMB(TM_IN + (p))
#define TM_DEPTH  32                /* arrays and inline tables in each other; E_NOMEM past it */
#define TM_END    0xFFF0U

/* Nodes. A table and an array: kind, format, first item, last item. An item:
 * its value, the next item, and (a table's) its key, a STRING. A STRING:
 * kind, length, bytes. INT and FLOAT: kind and 8 bytes. BOOL: kind, value. */
#define N_FMT(t)   TMB((t) + 1)
#define N_FIRST(t) TMW((t) + 2)
#define N_LAST(t)  TMW((t) + 4)
#define I_VAL(i)   TMW((i) + 0)
#define I_NEXT(i)  TMW((i) + 2)
#define I_KEY(i)   TMW((i) + 4)
#define S_LEN(s)   TMW((s) + 1)

/* table_format and array_format, as far as insert_value asks */
#define F_MULTILINE 0       /* [header], or the root */
#define F_ONELINE   1       /* { inline } */
#define F_DOTTED    2       /* made by a dotted key */
#define F_IMPLICIT  4       /* made by a header's key path */
#define F_AOT       3       /* [[array of tables]] */
#define F_INLINE    0       /* [ an array value ] */

#define K_DOTTED    0
#define K_STD       1
#define K_ARRAY     2

#define IS_DIGIT(ch) ((ch) >= '0' && (ch) <= '9')
#define IS_ALPHA(ch) (((ch) >= 'a' && (ch) <= 'z') || ((ch) >= 'A' && (ch) <= 'Z'))
#define IS_HEX(ch)   (IS_DIGIT(ch) || ((ch) >= 'a' && (ch) <= 'f') || ((ch) >= 'A' && (ch) <= 'F'))

static uint16_t alloc(uint16_t n)
{
    tm_t *c = TM;
    uint16_t a = c->top;
    if ((uint16_t)(TM_END - a) < n) tm_fail(TM_ESPACE);
    c->top = a + n;
    return a;
}

static uint16_t new_node(uint8_t kind, uint8_t fmt)
{
    uint16_t t = alloc(6);
    TMB(t) = kind;
    N_FMT(t) = fmt;
    N_FIRST(t) = 0;
    N_LAST(t) = 0;
    return t;
}

/* An item at the end of a table's or an array's list */
static void append(uint16_t t, uint16_t key, uint16_t v)
{
    uint16_t i = alloc(6);
    I_VAL(i) = v;
    I_NEXT(i) = 0;
    I_KEY(i) = key;
    if (N_LAST(t)) I_NEXT(N_LAST(t)) = i;
    else N_FIRST(t) = i;
    N_LAST(t) = i;
}

/* ---- the grammar (syntax_impl.hpp), TOML 1.0: each returns the end, or NONE ---- */

static int lit(uint16_t p, const char *s, uint16_t k)
{
    uint16_t i;
    if ((uint16_t)(TM->n - p) < k) return 0;
    for (i = 0; i != k; ++i) if (IN(p + i) != (uint8_t)s[i]) return 0;
    return 1;
}

static uint16_t s_ws(uint16_t p)
{
    uint16_t n = TM->n;
    while (p != n && (IN(p) == ' ' || IN(p) == '\t')) ++p;
    return p;
}

static uint16_t s_newline(uint16_t p)
{
    uint16_t n = TM->n;
    if (p != n && IN(p) == '\n') return p + 1;
    if ((uint16_t)(n - p) >= 2 && IN(p) == '\r' && IN(p + 1) == '\n') return p + 2;
    return NONE;
}

/* non_ascii: a UTF-8 sequence of 2 to 4 bytes, by the grammar's ranges */
static uint16_t s_nonascii(uint16_t p)
{
    uint16_t n = TM->n, len, i;
    uint8_t a, b, lo = 0x80, hi = 0xBF;
    if (p == n) return NONE;
    a = IN(p);
    if (a >= 0xC2 && a <= 0xDF) len = 2;
    else if (a == 0xE0) { len = 3; lo = 0xA0; }
    else if (a >= 0xE1 && a <= 0xEC) len = 3;
    else if (a == 0xED) { len = 3; hi = 0x9F; }
    else if (a >= 0xEE && a <= 0xEF) len = 3;
    else if (a == 0xF0) { len = 4; lo = 0x90; }
    else if (a >= 0xF1 && a <= 0xF3) len = 4;
    else if (a == 0xF4) { len = 4; hi = 0x8F; }
    else return NONE;
    if ((uint16_t)(n - p) < len) return NONE;
    b = IN(p + 1);
    if (b < lo || b > hi) return NONE;
    for (i = 2; i < len; ++i) {
        b = IN(p + i);
        if (b < 0x80 || b > 0xBF) return NONE;
    }
    return p + len;
}

/* comment: '#' and the characters a comment allows (1.0: tab, printable, non-ASCII) */
static uint16_t s_comment(uint16_t p)
{
    uint16_t n = TM->n, q;
    uint8_t ch;
    if (p == n || IN(p) != '#') return NONE;
    ++p;
    while (p != n) {
        ch = IN(p);
        if (ch == 0x09 || (ch >= 0x20 && ch <= 0x7E)) ++p;
        else if (ch >= 0x80 && (q = s_nonascii(p)) != NONE) p = q;
        else break;
    }
    return p;
}

static uint16_t s_hexes(uint16_t p, uint16_t k)
{
    uint16_t i;
    if ((uint16_t)(TM->n - p) < k) return NONE;
    for (i = 0; i != k; ++i) if (!IS_HEX(IN(p + i))) return NONE;
    return p + k;
}

/* escaped: a backslash and one of " \ b f n r t, uXXXX or UXXXXXXXX */
static uint16_t s_escaped(uint16_t p)
{
    uint16_t n = TM->n;
    uint8_t ch;
    if ((uint16_t)(n - p) < 2 || IN(p) != '\\') return NONE;
    ch = IN(p + 1);
    if (ch == '"' || ch == '\\' || ch == 'b' || ch == 'f' || ch == 'n' || ch == 'r' || ch == 't') return p + 2;
    if (ch == 'u') return s_hexes(p + 2, 4);
    if (ch == 'U') return s_hexes(p + 2, 8);
    return NONE;
}

/* basic_char: a basic string's unescaped character, or an escape */
static uint16_t s_basic_char(uint16_t p)
{
    uint8_t ch;
    if (p == TM->n) return NONE;
    ch = IN(p);
    if (ch == ' ' || ch == '\t' || ch == 0x21 || (ch >= 0x23 && ch <= 0x5B) || (ch >= 0x5D && ch <= 0x7E)) return p + 1;
    if (ch >= 0x80) return s_nonascii(p);
    if (ch == '\\') return s_escaped(p);
    return NONE;
}

static uint16_t s_basic_string(uint16_t p)
{
    uint16_t n = TM->n, q;
    if (p == n || IN(p) != '"') return NONE;
    ++p;
    while ((q = s_basic_char(p)) != NONE) p = q;
    if (p == n || IN(p) != '"') return NONE;
    return p + 1;
}

/* escaped_newline: a backslash, spaces, a newline, then spaces and newlines */
static uint16_t s_escaped_newline(uint16_t p, uint16_t end)
{
    uint16_t q;
    if (p == end || IN(p) != '\\') return NONE;
    p = s_ws(p + 1);
    if (p >= end) return NONE;
    q = s_newline(p);
    if (q == NONE || q > end) return NONE;
    p = q;
    for (;;) {
        if (p == end) return p;
        if (IN(p) == ' ' || IN(p) == '\t') { ++p; continue; }
        q = s_newline(p);
        if (q == NONE || q > end) return p;
        p = q;
    }
}

static uint16_t s_mlb_content(uint16_t p)
{
    uint16_t q = s_basic_char(p);
    if (q != NONE) return q;
    q = s_newline(p);
    if (q != NONE) return q;
    return s_escaped_newline(p, TM->n);
}

/* mlb_quotes and mll_quotes: two of q, or one */
static uint16_t s_quotes(uint16_t p, uint8_t qc)
{
    uint16_t n = TM->n;
    if ((uint16_t)(n - p) >= 2 && IN(p) == qc && IN(p + 1) == qc) return p + 2;
    if (p != n && IN(p) == qc) return p + 1;
    return NONE;
}

/* literal_char */
static uint16_t s_literal_char(uint16_t p)
{
    uint8_t ch;
    if (p == TM->n) return NONE;
    ch = IN(p);
    if (ch == 0x09 || (ch >= 0x20 && ch <= 0x26) || (ch >= 0x28 && ch <= 0x7E)) return p + 1;
    if (ch >= 0x80) return s_nonascii(p);
    return NONE;
}

static uint16_t s_mll_content(uint16_t p)
{
    uint16_t q = s_literal_char(p);
    if (q != NONE) return q;
    return s_newline(p);
}

static uint16_t s_literal_string(uint16_t p)
{
    uint16_t n = TM->n, q;
    if (p == n || IN(p) != '\'') return NONE;
    ++p;
    while ((q = s_literal_char(p)) != NONE) p = q;
    if (p == n || IN(p) != '\'') return NONE;
    return p + 1;
}

/* ml_basic_string and ml_literal_string: three quotes, maybe a newline, the
 * content, then runs of one or two quotes each followed by content, three
 * quotes, and maybe one or two more */
static uint16_t s_multiline(uint16_t p, uint8_t qc)
{
    uint16_t q, r;
    const char *three = qc == '"' ? "\"\"\"" : "'''";
    if (!lit(p, three, 3)) return NONE;
    p += 3;
    q = s_newline(p);
    if (q != NONE) p = q;
    for (;;) {
        q = qc == '"' ? s_mlb_content(p) : s_mll_content(p);
        if (q == NONE) break;
        p = q;
    }
    for (;;) {
        q = s_quotes(p, qc);
        if (q == NONE) break;
        r = qc == '"' ? s_mlb_content(q) : s_mll_content(q);
        if (r == NONE) break;
        for (;;) {
            q = qc == '"' ? s_mlb_content(r) : s_mll_content(r);
            if (q == NONE) break;
            r = q;
        }
        p = r;
    }
    if (!lit(p, three, 3)) return NONE;
    p += 3;
    q = s_quotes(p, qc);
    if (q != NONE) p = q;
    return p;
}

static uint16_t s_unquoted_key(uint16_t p)
{
    uint16_t n = TM->n, s = p;
    uint8_t ch;
    while (p != n) {
        ch = IN(p);
        if (IS_ALPHA(ch) || IS_DIGIT(ch) || ch == '-' || ch == '_') ++p;
        else break;
    }
    return p == s ? NONE : p;
}

static uint16_t s_simple_key(uint16_t p)
{
    uint16_t q = s_unquoted_key(p);
    if (q != NONE) return q;
    q = s_basic_string(p);
    if (q != NONE) return q;
    return s_literal_string(p);
}

/* key: a dotted key, or a simple one */
static uint16_t s_key(uint16_t p)
{
    uint16_t q = s_simple_key(p), r, e;
    if (q == NONE) return NONE;
    for (;;) {
        r = s_ws(q);
        if (r == TM->n || IN(r) != '.') return q;
        e = s_simple_key(s_ws(r + 1));
        if (e == NONE) return q;
        q = e;
    }
}

/* a digit of a base, then digits or an underscore and a digit */
static int is_d(uint8_t ch, uint8_t base)
{
    if (base == 10) return IS_DIGIT(ch);
    if (base == 16) return IS_HEX(ch);
    if (base == 8) return ch >= '0' && ch <= '7';
    return ch == '0' || ch == '1';
}

static uint16_t s_run(uint16_t p, uint8_t base)
{
    uint16_t n = TM->n;
    if (p == n || !is_d(IN(p), base)) return NONE;
    ++p;
    for (;;) {
        if (p != n && is_d(IN(p), base)) { ++p; continue; }
        if ((uint16_t)(n - p) >= 2 && IN(p) == '_' && is_d(IN(p + 1), base)) { p += 2; continue; }
        return p;
    }
}

/* dec_int: a sign, then a digit 1-9 and at least one more, or one digit */
static uint16_t s_dec_int(uint16_t p)
{
    uint16_t n = TM->n, q, r;
    if (p != n && (IN(p) == '+' || IN(p) == '-')) ++p;
    if (p == n || !IS_DIGIT(IN(p))) return NONE;
    if (IN(p) != '0') {
        q = p + 1;
        r = q;
        for (;;) {
            if (r != n && IS_DIGIT(IN(r))) { ++r; continue; }
            if ((uint16_t)(n - r) >= 2 && IN(r) == '_' && IS_DIGIT(IN(r + 1))) { r += 2; continue; }
            break;
        }
        if (r != q) return r;
    }
    return p + 1;
}

static uint16_t s_prefixed(uint16_t p, uint8_t letter, uint8_t base)
{
    if ((uint16_t)(TM->n - p) < 2 || IN(p) != '0' || IN(p + 1) != letter) return NONE;
    return s_run(p + 2, base);
}

static uint16_t s_integer(uint16_t p)
{
    uint16_t q = s_prefixed(p, 'x', 16);
    if (q != NONE) return q;
    q = s_prefixed(p, 'o', 8);
    if (q != NONE) return q;
    q = s_prefixed(p, 'b', 2);
    if (q != NONE) return q;
    return s_dec_int(p);
}

static uint16_t s_exponent(uint16_t p)
{
    uint16_t n = TM->n;
    if (p == n || (IN(p) != 'e' && IN(p) != 'E')) return NONE;
    ++p;
    if (p != n && (IN(p) == '+' || IN(p) == '-')) ++p;
    return s_run(p, 10);
}

static uint16_t s_floating(uint16_t p)
{
    uint16_t n = TM->n, q = s_dec_int(p), r, e;
    if (q != NONE) {
        r = s_exponent(q);
        if (r != NONE) return r;
        if (q != n && IN(q) == '.' && (r = s_run(q + 1, 10)) != NONE) {
            e = s_exponent(r);
            if (e != NONE) return e;
            return r;
        }
    }
    q = p;
    if (q != n && (IN(q) == '+' || IN(q) == '-')) ++q;
    if (lit(q, "inf", 3) || lit(q, "nan", 3)) return q + 3;
    return NONE;
}

/* ---- the parser (parser.hpp) ---- */

/* skip_multiline_spacer: comments with their newline, newlines, spaces and
 * tabs. TM->nl says whether a newline was among them. */
static uint16_t spacer(uint16_t p)
{
    tm_t *c = TM;
    uint16_t n = c->n, q;
    uint8_t nl = 0;
    while (p != n) {
        q = s_comment(p);
        if (q != NONE && (q = s_newline(q)) != NONE) { p = q; nl = 1; continue; }
        q = s_newline(p);
        if (q != NONE) { p = q; nl = 1; continue; }
        if (IN(p) == ' ' || IN(p) == '\t') { ++p; continue; }
        break;
    }
    c->nl = nl;
    return p;
}

/* parse_comment_line: spaces and a comment, which a newline or the end must
 * follow; or nothing, and the spaces are not taken. TM->has says which. */
static uint16_t comment_line(uint16_t p)
{
    tm_t *c = TM;
    uint16_t q = s_comment(s_ws(p)), r;
    if (q != NONE) {
        if (q != c->n) {
            r = s_newline(q);
            if (r == NONE) tm_fail(TM_EINVAL);
            q = r;
        }
        c->has = 1;
        return q;
    }
    c->has = 0;
    return p;
}

/* A STRING node of up to n bytes; its length is set by the caller */
static uint16_t new_string(uint16_t n)
{
    uint16_t s = alloc(n + 3);
    TMB(s) = TM_STRING;
    return s;
}

static void put_byte(uint16_t s, uint16_t *len, uint8_t b)
{
    TMB(s + 3 + *len) = b;
    ++*len;
}

/* parse_escape_sequence, the syntax already checked: at p, a backslash */
static uint16_t escape(uint16_t p, uint16_t s, uint16_t *len)
{
    uint8_t ch = IN(p + 1);
    uint16_t k, i;
    uint32_t cp = 0;
    switch (ch) {
    case 'b': put_byte(s, len, '\b'); return p + 2;
    case 'f': put_byte(s, len, '\f'); return p + 2;
    case 'n': put_byte(s, len, '\n'); return p + 2;
    case 'r': put_byte(s, len, '\r'); return p + 2;
    case 't': put_byte(s, len, '\t'); return p + 2;
    case 'u': case 'U':
        k = ch == 'u' ? 4 : 8;
        for (i = 0; i != k; ++i) {
            ch = IN(p + 2 + i);
            cp = (cp << 4) | (uint32_t)(IS_DIGIT(ch) ? ch - '0' : (ch | 0x20) - 'a' + 10);
        }
        if (cp < 0x80) put_byte(s, len, (uint8_t)cp);
        else if (cp < 0x800) {
            put_byte(s, len, (uint8_t)(0xC0 | (cp >> 6)));
            put_byte(s, len, (uint8_t)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            if (cp >= 0xD800 && cp <= 0xDFFF) tm_fail(TM_EINVAL);
            put_byte(s, len, (uint8_t)(0xE0 | (cp >> 12)));
            put_byte(s, len, (uint8_t)(0x80 | ((cp >> 6) & 0x3F)));
            put_byte(s, len, (uint8_t)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x110000UL) {
            put_byte(s, len, (uint8_t)(0xF0 | (cp >> 18)));
            put_byte(s, len, (uint8_t)(0x80 | ((cp >> 12) & 0x3F)));
            put_byte(s, len, (uint8_t)(0x80 | ((cp >> 6) & 0x3F)));
            put_byte(s, len, (uint8_t)(0x80 | (cp & 0x3F)));
        } else tm_fail(TM_EINVAL);
        return p + 2 + k;
    default:                                /* " or \ */
        put_byte(s, len, ch);
        return p + 2;
    }
}

/* The bytes from..end as a STRING, decoded: escapes in a basic string, and
 * in a multi-line one escaped newlines too; none in a literal one */
static uint16_t decode(uint16_t from, uint16_t end, uint8_t basic, uint8_t ml)
{
    uint16_t s = new_string(end - from), len = 0, q;
    while (from != end) {
        if (basic && IN(from) == '\\') {
            if (ml && (q = s_escaped_newline(from, end)) != NONE) from = q;
            else from = escape(from, s, &len);
        } else put_byte(s, &len, IN(from++));
    }
    S_LEN(s) = len;
    TM->top = s + 3 + len;
    return s;
}

/* parse_string, at a quote: TM->val the STRING */
static uint16_t parse_string(uint16_t p)
{
    uint16_t q, from;
    uint8_t qc = IN(p), basic = qc == '"';
    if (lit(p, basic ? "\"\"\"" : "'''", 3)) {
        q = s_multiline(p, qc);
        if (q == NONE) tm_fail(TM_EINVAL);
        from = p + 3;                       /* the region less three quotes at each end */
        if (IN(from) == '\n') ++from;
        else if (IN(from) == '\r' && IN(from + 1) == '\n') from += 2;
        if (from > q - 3) from = q - 3;
        TM->val = decode(from, q - 3, basic, 1);
        return q;
    }
    q = basic ? s_basic_string(p) : s_literal_string(p);
    if (q == NONE) tm_fail(TM_EINVAL);
    TM->val = decode(p + 1, q - 1, basic, 0);
    return q;
}

/* parse_simple_key: TM->val its STRING */
static uint16_t simple_key(uint16_t p)
{
    uint16_t q, s;
    if (p != TM->n && (IN(p) == '"' || IN(p) == '\'')) {
        q = IN(p) == '"' ? s_basic_string(p) : s_literal_string(p);
        if (q == NONE) tm_fail(TM_EINVAL);
        TM->val = decode(p + 1, q - 1, IN(p) == '"', 0);
        return q;
    }
    q = s_unquoted_key(p);
    if (q == NONE) tm_fail(TM_EINVAL);
    s = decode(p, q, 0, 0);
    TM->val = s;
    return q;
}

/* parse_key: the keys are consecutive STRING nodes from *k0, *nk of them */
static uint16_t parse_key(uint16_t p, uint16_t *k0, uint16_t *nk)
{
    uint16_t n = TM->n, q;
    *nk = 0;
    while (p != n) {
        p = simple_key(p);
        if (!*nk) *k0 = TM->val;
        ++*nk;
        q = s_ws(p);
        if (q == n || IN(q) != '.') break;
        p = s_ws(q + 1);
    }
    if (!*nk) tm_fail(TM_EINVAL);
    return p;
}

#define NEXT_KEY(k) ((k) + 3 + S_LEN(k))

static int key_eq(uint16_t a, uint16_t b)
{
    uint16_t n = S_LEN(a);
    return n == S_LEN(b) && memcmp(TM_P(a + 3), TM_P(b + 3), n) == 0;
}

static uint16_t find(uint16_t t, uint16_t key)
{
    uint16_t i;
    for (i = N_FIRST(t); i; i = I_NEXT(i)) if (key_eq(I_KEY(i), key)) return i;
    return 0;
}

/* is_array_of_tables: not empty, and every element a table */
static int is_aot(uint16_t a)
{
    uint16_t i;
    if (TMB(a) != TM_ARRAY || !N_FIRST(a)) return 0;
    for (i = N_FIRST(a); i; i = I_NEXT(i)) if (TMB(I_VAL(i)) != TM_TABLE) return 0;
    return 1;
}

/* insert_value: the keys from k0 into table t; for a dotted key the value v,
 * for a header a fresh table, which TM->val is then */
static void insert(uint8_t kind, uint16_t t, uint16_t k0, uint16_t nk, uint16_t v)
{
    uint16_t k = k0, i, f, tab;
    uint8_t fmt;
    for (; nk > 1; --nk, k = NEXT_KEY(k)) {
        i = find(t, k);
        if (!i) {
            f = new_node(TM_TABLE, kind == K_DOTTED ? F_DOTTED : F_IMPLICIT);
            append(t, k, f);
            t = f;
            continue;
        }
        f = I_VAL(i);
        if (TMB(f) == TM_TABLE) {
            fmt = N_FMT(f);
            if (fmt == F_ONELINE) tm_fail(TM_EINVAL);           /* an inline table is immutable */
            if (kind == K_DOTTED && fmt != F_DOTTED) tm_fail(TM_EINVAL);
            t = f;
        } else if (is_aot(f)) {
            if (N_FMT(f) != F_AOT) tm_fail(TM_EINVAL);          /* an inline array of tables */
            if (kind == K_DOTTED) tm_fail(TM_EINVAL);
            t = I_VAL(N_LAST(f));
        } else tm_fail(TM_EINVAL);
    }
    i = find(t, k);
    if (kind == K_DOTTED) {
        if (i) tm_fail(TM_EINVAL);
        append(t, k, v);
        return;
    }
    if (kind == K_STD) {
        if (!i) {
            tab = new_node(TM_TABLE, F_MULTILINE);
            append(t, k, tab);
            TM->val = tab;
            return;
        }
        f = I_VAL(i);
        if (TMB(f) != TM_TABLE || N_FMT(f) != F_IMPLICIT) tm_fail(TM_EINVAL);
        N_FMT(f) = F_MULTILINE;             /* reopened: defined now */
        TM->val = f;
        return;
    }
    tab = new_node(TM_TABLE, F_MULTILINE);
    if (!i) {
        f = new_node(TM_ARRAY, F_AOT);
        append(t, k, f);
    } else {
        f = I_VAL(i);
        if (!is_aot(f) || N_FMT(f) != F_AOT) tm_fail(TM_EINVAL);
    }
    append(f, 0, tab);
    TM->val = tab;
}

static uint16_t parse_value(uint16_t p);

/* parse_key_value_pair: keys, ws = ws, a value (TM->val) */
static uint16_t kv_pair(uint16_t p, uint16_t *k0, uint16_t *nk)
{
    p = parse_key(p, k0, nk);
    p = s_ws(p);
    if (p == TM->n || IN(p) != '=') tm_fail(TM_EINVAL);
    return parse_value(s_ws(p + 1));
}

static void open_nesting(void)
{
    if (++TM->depth > TM_DEPTH) tm_fail(TM_ESPACE);
}

/* parse_array, at '[' */
static uint16_t parse_array(uint16_t p)
{
    tm_t *c = TM;
    uint16_t a = new_node(TM_ARRAY, F_INLINE), n = c->n;
    uint8_t comma = 1;
    open_nesting();
    p = spacer(p + 1);
    while (p != n) {
        if (IN(p) == ']') break;
        if (!comma) tm_fail(TM_EINVAL);
        p = parse_value(p);
        append(a, 0, c->val);
        p = spacer(p);
        comma = p != n && IN(p) == ',';
        if (comma) ++p;
        p = comment_line(p);
        if (comma) p = spacer(p);
    }
    if (p == n || IN(p) != ']') tm_fail(TM_EINVAL);
    --c->depth;
    c->val = a;
    return p + 1;
}

/* parse_inline_table, at '{', TOML 1.0: one line, no trailing comma */
static uint16_t parse_inline_table(uint16_t p)
{
    tm_t *c = TM;
    uint16_t t = new_node(TM_TABLE, F_ONELINE), n = c->n, k0, nk;
    uint8_t empty = 1, comma = 0;
    open_nesting();
    p = s_ws(p + 1);
    while (p != n) {
        if (IN(p) == '}') {
            if (comma) tm_fail(TM_EINVAL);
            break;
        }
        if (!comma && !empty) tm_fail(TM_EINVAL);
        empty = 0;
        p = kv_pair(p, &k0, &nk);
        insert(K_DOTTED, t, k0, nk, c->val);
        p = s_ws(p);
        comma = p != n && IN(p) == ',';
        if (comma) ++p;
        p = s_ws(p);
    }
    if (p == n || IN(p) != '}') tm_fail(TM_EINVAL);
    --c->depth;
    c->val = t;
    return p + 1;
}

static uint16_t new_num(uint8_t kind, const uint8_t *b)
{
    uint16_t v = alloc(9);
    TMB(v) = kind;
    memcpy(TM_P(v + 1), b, 8);
    return v;
}

/* The digits from p to end, their underscores gone, at the arena's top
 * (scratch, not kept): their count */
static uint16_t digits(uint16_t p, uint16_t end, uint16_t *at)
{
    uint16_t d = TM->top, k = 0;
    if ((uint16_t)(TM_END - d) < (uint16_t)(end - p)) tm_fail(TM_ESPACE);
    for (; p != end; ++p) if (IN(p) != '_') TMB(d + k++) = IN(p);
    *at = d;
    return k;
}

/* read_int: an int64 of base 10, 16, 8 or 2, or E: istringstream's failbit
 * past INT64 for 10, 16 and 8; for 2, more than 63 significant bits */
static uint16_t int_value(uint16_t p, uint16_t end, uint8_t base)
{
    uint16_t d, k, i;
    int64_t v = 0;
    uint8_t ch, x, neg = 0;
    k = digits(p, end, &d);
    i = 0;
    if (base == 10 && (TMB(d) == '+' || TMB(d) == '-')) { neg = TMB(d) == '-'; i = 1; }
    if (base != 10) i = 2;
    for (; i != k; ++i) {
        ch = TMB(d + i);
        x = IS_DIGIT(ch) ? ch - '0' : (ch | 0x20) - 'a' + 10;
        if (base == 10) {
            /* 10v +- x within INT64, accumulated on the side of the sign */
            if (neg ? v < -922337203685477580LL || (v == -922337203685477580LL && x > 8)
                    : v > 922337203685477580LL || (v == 922337203685477580LL && x > 7)) tm_fail(TM_EINVAL);
            v = v + v;
            v = v + v + v + v + v;
            v = neg ? v - x : v + x;
        } else if (base == 16) {
            if (v > 0x07FFFFFFFFFFFFFFLL) tm_fail(TM_EINVAL);
            v = (v << 4) | x;
        } else if (base == 8) {
            if (v > 0x0FFFFFFFFFFFFFFFLL) tm_fail(TM_EINVAL);
            v = (v << 3) | x;
        } else {
            if (v > 0x3FFFFFFFFFFFFFFFLL) tm_fail(TM_EINVAL);
            v = (v << 1) | x;
        }
    }
    TM->val = new_num(TM_INT, (const uint8_t *)&v);
    return end;
}

static uint16_t float_value(uint16_t p, uint16_t end)
{
    uint16_t d, k, s = 0;
    uint64_t b;
    uint8_t neg = 0, ch;
    k = digits(p, end, &d);
    ch = TMB(d);
    if (ch == '+' || ch == '-') { neg = ch == '-'; s = 1; }
    if (TMB(d + s) == 'i') b = 0x7FF0000000000000ULL;
    else if (TMB(d + s) == 'n') b = 0x7FF8000000000000ULL;
    else {
        b = f_strtod((const char *)TM_P(d + s), k - s);   /* JSON's grammar, but for the sign */
        if ((b & 0x7FFFFFFFFFFFFFFFULL) == 0x7FF0000000000000ULL) tm_fail(TM_EINVAL);  /* overflow: failbit */
    }
    if (neg) b ^= 0x8000000000000000ULL;
    TM->val = new_num(TM_FLOAT, (const uint8_t *)&b);
    return end;
}

/* guess_number_type, then parse_floating or parse_integer */
static uint16_t number(uint16_t p)
{
    uint16_t n = TM->n, q, s;
    uint8_t ch;
    q = s_floating(p);
    if (q != NONE) {
        if (q != n && IN(q) == '_') tm_fail(TM_EINVAL);
        return float_value(p, q);
    }
    q = s_integer(p);
    if (q == NONE) tm_fail(TM_EINVAL);
    if (q != n) {
        ch = IN(q);
        if (ch == '_' || IS_DIGIT(ch) || ch == ':' || ch == '-' || ch == '.' || ch == 'e' || ch == 'E')
            tm_fail(TM_EINVAL);             /* a date or a time among them */
    }
    s = p;
    if (IN(s) == '+' || IN(s) == '-') ++s;
    if (IN(s) == '0' && s + 1 != n) {       /* parse_integer: the prefix, from the start */
        ch = IN(s + 1);
        if (ch == 'b' || ch == 'o' || ch == 'x') {
            q = s_prefixed(p, ch, ch == 'b' ? 2 : ch == 'o' ? 8 : 16);
            if (q == NONE) tm_fail(TM_EINVAL);
            return int_value(p, q, ch == 'b' ? 2 : ch == 'o' ? 8 : 16);
        }
    }
    q = s_dec_int(p);
    return int_value(p, q, 10);
}

/* parse_value, by guess_value_type: TM->val the node */
static uint16_t parse_value(uint16_t p)
{
    uint16_t v;
    uint8_t ch;
    if (p == TM->n) tm_fail(TM_EINVAL);
    ch = IN(p);
    switch (ch) {
    case '"': case '\'': return parse_string(p);
    case '[': return parse_array(p);
    case '{': return parse_inline_table(p);
    case 't': case 'f':
        if (!lit(p, ch == 't' ? "true" : "false", ch == 't' ? 4 : 5)) tm_fail(TM_EINVAL);
        v = alloc(2);
        TMB(v) = TM_BOOL;
        TMB(v + 1) = ch == 't';
        TM->val = v;
        return p + (ch == 't' ? 4 : 5);
    case 'i': case 'n':
        if (!lit(p, ch == 'i' ? "inf" : "nan", 3)) tm_fail(TM_EINVAL);
        return number(p);
    case 'T': case 'F': case 'I': case 'N':
        tm_fail(TM_EINVAL);
    default:
        return number(p);
    }
}

/* parse_table: key-value pairs into t, each after a newline, up to a header */
static uint16_t parse_table(uint16_t p, uint16_t t)
{
    tm_t *c = TM;
    uint16_t n = c->n, start, k0, nk, v;
    uint8_t nlf = 1;
    while (p != n) {
        start = p;
        p = spacer(p);
        if (p == n) break;
        v = s_ws(p);
        if (v != n && IN(v) == '[') { p = start; break; }
        nlf = nlf || c->nl;
        if (!nlf) tm_fail(TM_EINVAL);
        nlf = 0;
        p = kv_pair(p, &k0, &nk);
        v = c->val;
        p = comment_line(p);
        if (c->has) nlf = 1;
        insert(K_DOTTED, t, k0, nk, v);
    }
    return p;
}

/* A header's end: a comment line, or spaces and a newline (or the end) */
static uint16_t header_end(uint16_t p)
{
    uint16_t q;
    p = comment_line(p);
    if (TM->has) return p;
    p = s_ws(p);
    if (p == TM->n) return p;
    q = s_newline(p);
    if (q == NONE) tm_fail(TM_EINVAL);
    return q;
}

/* [[key]] or [key] at p: the keys, and the position after the header; or NONE */
static uint16_t header(uint16_t p, uint8_t aot, uint16_t *k0, uint16_t *nk)
{
    uint16_t w = aot ? 2 : 1, q;
    if (!lit(p, aot ? "[[" : "[", w)) return NONE;
    q = s_key(s_ws(p + w));
    if (q == NONE) return NONE;
    q = s_ws(q);
    if (!lit(q, aot ? "]]" : "]", w)) return NONE;
    q = parse_key(s_ws(p + w), k0, nk);
    return s_ws(q) + w;
}

uint16_t tm_parse(const char *s, uint16_t n0)
{
    tm_t *c = TM;
    uint16_t n = n0, root, p, first, q, k0, nk;
    if (n0 > TM_END - TM_IN - 1) tm_fail(TM_ESPACE);
    memcpy(TM_P(TM_IN), s, n0);
    if (n0 && IN(n0 - 1) != '\n' && IN(n0 - 1) != '\r') IN(n++) = '\n';
    c->n = n;
    c->top = TM_IN + n;
    c->depth = 0;
    root = new_node(TM_TABLE, F_MULTILINE);
    if (!n0) return root;
    p = 0;
    if (n >= 3 && IN(0) == 0xEF && IN(1) == 0xBB && IN(2) == 0xBF) p = 3;
    first = p;
    while (p != n) {                        /* the comments before the first table */
        q = comment_line(p);
        if (c->has) { p = q; continue; }
        q = s_newline(s_ws(p));
        p = q == NONE ? first : q;
        break;
    }
    p = parse_table(p, root);
    while (p != n) {
        p = spacer(p);
        q = header(p, 1, &k0, &nk);
        if (q != NONE) {
            p = header_end(q);
            insert(K_ARRAY, root, k0, nk, 0);
            p = parse_table(p, c->val);
            continue;
        }
        q = header(p, 0, &k0, &nk);
        if (q == NONE) tm_fail(TM_EINVAL);
        p = header_end(q);
        insert(K_STD, root, k0, nk, 0);
        p = parse_table(p, c->val);
    }
    return root;
}

/* ---- the tree, for the caller ---- */

uint8_t  tm_kind(uint16_t node) { return TMB(node); }
uint16_t tm_first(uint16_t node) { return N_FIRST(node); }
uint16_t tm_next(uint16_t item) { return I_NEXT(item); }
uint16_t tm_val(uint16_t item) { return I_VAL(item); }
uint16_t tm_key(uint16_t entry) { return I_KEY(entry); }
uint16_t tm_len(uint16_t node) { return S_LEN(node); }
const uint8_t *tm_bytes(uint16_t node) { return TM_P(node + (TMB(node) == TM_STRING ? 3 : 1)); }
uint8_t  tm_bool(uint16_t node) { return TMB(node + 1); }

/* A table's entries relinked in the order of their keys' bytes (shorter
 * first on a tie), Nix's order for a set: an insertion sort of the list. */
static int key_lt(uint16_t a, uint16_t b)
{
    uint16_t la = S_LEN(a), lb = S_LEN(b);
    int c = memcmp(TM_P(a + 3), TM_P(b + 3), la < lb ? la : lb);
    return c < 0 || (c == 0 && la < lb);
}

void tm_sort(uint16_t t)
{
    uint16_t sorted = 0, i, nx, *pp, q;
    for (i = N_FIRST(t); i; i = nx) {
        nx = I_NEXT(i);
        pp = &sorted;
        for (q = sorted; q && !key_lt(I_KEY(i), I_KEY(q)); q = I_NEXT(q)) pp = (uint16_t *)TM_P(q + 2);
        I_NEXT(i) = q;
        *pp = i;
    }
    N_FIRST(t) = sorted;
}
