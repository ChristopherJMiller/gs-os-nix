/* lex.c -- rnix 0.14.0's tokenizer, token for token (docs/COMPILER.md
 * "Stage 1"): the same tokens -- trivia and errors too -- over the same
 * text, so that the parser sees what rnix's does. Each function is the one
 * of the same name in tokenizer.rs, and its control flow is that one's.
 *
 * rnix steps by Unicode character. Its tests are against ASCII characters,
 * which a UTF-8 continuation or lead byte never is, so stepping by byte
 * finds the same boundaries -- except where it decides on a character that
 * may not be ASCII: an unknown one is an error token of its whole sequence
 * (next), whitespace is Unicode White_Space (ws_len), and the character
 * after a path's `/` must not be whitespace (next_inner). Those step by
 * sequence. lex_init refuses text that is not UTF-8, as nbc does.
 */
#pragma noroot
#pragma optimize 15

#include "vm.h"
#include "front.h"

segment "front";

static const uint8_t *src;
static uint16_t slen;
uint16_t lex_off;
#define off lex_off

/* The context stack (tokenizer.rs Context): string bodies, a string's end,
 * interpolations with their open braces, paths with their kind. In the
 * front end's bank: a string inside an interpolation inside a string is two
 * entries a level, and nothing bounds the nesting but the text. */
enum { C_STRING_BODY, C_STRING_END, C_INTERPOL, C_INTERPOL_START, C_PATH };
struct lex_ctx { uint8_t kind, sub; uint16_t braces; };   /* sub: multiline, or a path's kind */
#define LEX_CTX   ((struct lex_ctx *)FRONT_BASE)
#define LEX_DEPTH 4096U
static uint16_t csp;

static void push_ctx(uint8_t kind, uint8_t sub)
{
    if (csp >= LEX_DEPTH) nomem();
    LEX_CTX[csp].kind = kind;
    LEX_CTX[csp].sub = sub;
    LEX_CTX[csp].braces = 0;
    ++csp;
}
#define pop_ctx()  (--csp)
#define top_is(k)  (csp && LEX_CTX[csp - 1].kind == (k))

/* The byte at off + k, or -1 past the end. */
static int at(uint16_t k)
{
    uint16_t i = off + k;
    return i < slen && i >= off ? src[i] : -1;
}
#define cur() at(0)                         /* rnix's peek(); vm.h has a peek() of its own */

/* A UTF-8 sequence's length from its first byte (the text is valid UTF-8). */
static uint16_t ulen(uint8_t b)
{
    return b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
}

/* One character: its first byte, or -1 at the end. */
static int next(void)
{
    int c = cur();
    if (c >= 0) off += ulen((uint8_t)c);
    return c;
}

static int starts_with_bump(const char *s, uint16_t n)
{
    uint16_t i;
    for (i = 0; i < n; ++i)
        if (at(i) != (uint8_t)s[i]) return 0;
    off += n;
    return 1;
}

/* The length of the White_Space character at off + k, 0 if it is not one
 * (Rust's char::is_whitespace): U+0009-000D, 0020, 0085, 00A0, 1680,
 * 2000-200A, 2028, 2029, 202F, 205F, 3000. */
static uint16_t ws_len(uint16_t k)
{
    int b = at(k), b1, b2;
    if (b < 0) return 0;
    if (b == ' ' || (b >= 0x09 && b <= 0x0D)) return 1;
    if (b < 0xC2) return 0;
    b1 = at(k + 1);
    if (b == 0xC2) return (b1 == 0x85 || b1 == 0xA0) ? 2 : 0;
    b2 = at(k + 2);
    if (b == 0xE1) return (b1 == 0x9A && b2 == 0x80) ? 3 : 0;
    if (b == 0xE2 && b1 == 0x80)
        return ((b2 >= 0x80 && b2 <= 0x8A) || b2 == 0xA8 || b2 == 0xA9 || b2 == 0xAF) ? 3 : 0;
    if (b == 0xE2) return (b1 == 0x81 && b2 == 0x9F) ? 3 : 0;
    if (b == 0xE3) return (b1 == 0x80 && b2 == 0x80) ? 3 : 0;
    return 0;
}

static int is_valid_path_char(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '/' || c == '_' || c == '.' || c == '+' || c == '-';
}

static int is_valid_uri_char(int c)
{
    switch (c) {
    case '%': case '?': case ':': case '@': case '&': case '=': case '$': case ',':
    case '!': case '~': case '*': case '\'':
        return 1;
    }
    return is_valid_path_char(c);
}

static int is_digit(int c) { return c >= '0' && c <= '9'; }

static uint16_t consume_digits(void)
{
    uint16_t n = 0;
    while (is_digit(cur())) { ++off; ++n; }
    return n;
}

static void consume_path(void)
{
    while (is_valid_path_char(cur())) ++off;
}

static uint8_t next_string(uint8_t multiline)
{
    uint16_t start;
    int c;
    for (;;) {
        start = off;
        c = next();
        if (c < 0) {
            pop_ctx();
            return TK_ERROR;
        }
        if (c == '"' && !multiline) {
            off = start;
            pop_ctx();
            push_ctx(C_STRING_END, 0);
            return TK_STRING_CONTENT;
        }
        if (c == '\\' && !multiline) {
            if (next() < 0) return TK_ERROR;
        } else if (c == '\'' && multiline) {
            c = cur();
            if (c < 0) return TK_ERROR;
            if (c == '\'') {
                next();
                c = cur();
                if (c == '\'' || c == '$') {
                    next();
                } else if (c == '\\') {
                    next();
                    if (next() < 0) return TK_ERROR;
                } else {
                    off = start;
                    pop_ctx();
                    push_ctx(C_STRING_END, 0);
                    return TK_STRING_CONTENT;
                }
            }
        } else if (c == '$') {
            c = cur();
            if (c == '$') {
                next();
            } else if (c == '{') {
                off = start;
                push_ctx(C_INTERPOL_START, 0);
                return TK_STRING_CONTENT;
            }
        }
    }
}

static uint8_t check_path_since(uint16_t past, uint8_t kind)
{
    uint16_t i;
    consume_path();
    if (at(0) == '$' && at(1) == '{') {
        push_ctx(C_INTERPOL_START, 0);
    } else {
        if (src[off - 1] == '/') return TK_ERROR;              /* ends with '/' */
        for (i = past; i + 1 < off; ++i)
            if (src[i] == '/' && src[i + 1] == '/') return TK_ERROR;   /* contains "//" */
        pop_ctx();
    }
    return kind;
}

/* tokenizer.rs IdentType */
enum { ID_NONE, ID_IDENT, ID_PATH_REL, ID_PATH_ABS, ID_PATH_HOME, ID_PATH_SEARCH, ID_URI };

static int is_word(const char *w, uint16_t start)
{
    uint16_t i, n = (uint16_t)(off - start);
    for (i = 0; i < n; ++i)
        if (!w[i] || src[start + i] != (uint8_t)w[i]) return 0;
    return w[n] == 0;
}

static uint8_t keyword(uint16_t start)
{
    switch (src[start]) {
    case 'a': if (is_word("assert", start)) return TK_ASSERT; break;
    case 'e': if (is_word("else", start)) return TK_ELSE; break;
    case 'i':
        if (is_word("if", start)) return TK_IF;
        if (is_word("in", start)) return TK_IN;
        if (is_word("inherit", start)) return TK_INHERIT;
        break;
    case 'l': if (is_word("let", start)) return TK_LET; break;
    case 'o': if (is_word("or", start)) return TK_OR; break;     /* contextual: the parser decides */
    case 'r': if (is_word("rec", start)) return TK_REC; break;
    case 't': if (is_word("then", start)) return TK_THEN; break;
    case 'w': if (is_word("with", start)) return TK_WITH; break;
    }
    return TK_IDENT;
}

static uint8_t consume_scientific(void)
{
    int c = cur();
    if (c == 'e' || c == 'E') {
        next();
        c = cur();
        if (c == '-' || c == '+') next();
        if (consume_digits() == 0) return TK_ERROR;
    }
    return TK_FLOAT;
}

/* The heuristic that decides, before the first character is taken, whether
 * the token is a path, a search path or a URI (tokenizer.rs, "Check if it's
 * a path"). */
static uint8_t ident_type(void)
{
    uint16_t i, j;
    int c, c1, c2, store, underscore = 0;
    c = cur();
    if (c == '/') {
        if (at(1) == '/') return ID_NONE;                  /* an update operator */
        if (at(1) == '$' && at(2) == '{') return ID_PATH_ABS;
        c1 = at(1);                                        /* the 2nd character: after an ASCII '/' */
        return (is_valid_path_char(c1) && c1 != '*') ? ID_PATH_ABS : ID_NONE;
    }
    if (c == '~' && at(1) == '/') return ID_PATH_HOME;
    store = (c == '<');
    for (i = 0; ; ++i) {                                   /* `skipped`: ASCII, so bytes are chars */
        c = at(i);
        if (c == '<' || c == '/') { if (!store) break; }
        else if (!is_valid_path_char(c)) break;
        if (c == '_') underscore = 1;
    }
    c1 = at(i);
    if (c1 < 0) return ID_NONE;
    j = (uint16_t)(i + ulen((uint8_t)c1));                 /* the character after it */
    c2 = at(j);
    if (c1 == '/') {
        if (c2 == '/' || c2 == '*') return ID_NONE;
        if (c2 >= 0 && !ws_len(j)) return ID_PATH_REL;
        return ID_NONE;
    }
    if (c1 == '>') return ID_PATH_SEARCH;
    if (c1 == ':' && c2 >= 0 && is_valid_uri_char(c2) && !underscore) return ID_URI;
    return ID_NONE;
}

static uint8_t next_inner(void)
{
    uint16_t start = off, n;
    uint8_t kind, sub;
    int c, ok;

    /* Handle an already started multi-token */
    for (;;) {
        if (!csp) break;
        kind = LEX_CTX[csp - 1].kind;
        sub = LEX_CTX[csp - 1].sub;
        if (kind == C_INTERPOL_START) {
            pop_ctx();
            push_ctx(C_INTERPOL, 0);
            off += 2;                                      /* "${": unreachable otherwise */
            return TK_INTERPOL_START;
        }
        if (kind == C_PATH) {
            if (sub == TK_PATH_SEARCH) {                   /* <nixpkgs> takes no interpolation */
                if (is_valid_path_char(cur())) return check_path_since(start, sub);
                pop_ctx();
            } else if (starts_with_bump("${", 2)) {
                push_ctx(C_INTERPOL, 0);
                return TK_INTERPOL_START;
            } else if (is_valid_path_char(cur())) {
                return check_path_since(start, sub);
            } else {
                pop_ctx();
            }
        } else if (kind == C_STRING_BODY) {
            kind = next_string(sub);
            if (off == start) continue;                    /* skip empty stuff */
            return kind;
        } else if (kind == C_STRING_END) {
            pop_ctx();
            c = cur();
            ok = 0;
            if (c == '"') {
                next();
                ok = 1;
            } else if (c == '\'') {
                next();
                if (cur() == '\'') { next(); ok = 1; }
            }
            return ok ? TK_STRING_END : TK_ERROR;
        }
        break;
    }

    n = 0;
    for (;;) { uint16_t w = ws_len(0); if (!w) break; off += w; n += w; }
    if (n) return TK_WHITESPACE;

    if (cur() == '#') {
        while ((c = cur()) >= 0 && c != '\n') ++off;
        return TK_COMMENT;
    }
    if (starts_with_bump("/*", 2)) {
        for (;;) {
            while ((c = cur()) >= 0 && c != '*') ++off;
            next();                                        /* the '*', if any */
            c = cur();
            if (c < 0) return TK_ERROR;
            if (c == '/') { next(); return TK_COMMENT; }
        }
    }
    if (starts_with_bump("...", 3)) return TK_ELLIPSIS;

    kind = ident_type();
    c = next();
    if (c < 0) return TK_EOF;

    if (c == '/' && cur() == '/') {                       /* the update operator */
        next();
        return TK_UPDATE;
    }
    if (c == '~' || kind == ID_PATH_ABS || kind == ID_PATH_REL || (c == '/' && is_valid_path_char(cur()))) {
        if (c == '~') {
            if (next() != '/') return TK_ERROR;
            sub = TK_PATH_HOME;
        } else {
            sub = (c == '/') ? TK_PATH_ABS : TK_PATH_REL;
        }
        push_ctx(C_PATH, sub);
        return check_path_since(start, sub);
    }

    switch (c) {
    case '=':
        if (cur() == '=') { next(); return TK_EQUAL; }
        return TK_ASSIGN;
    case '!':
        if (cur() == '=') { next(); return TK_NOT_EQUAL; }
        return TK_INVERT;
    case '{':
        if (top_is(C_INTERPOL)) ++LEX_CTX[csp - 1].braces;
        return TK_L_BRACE;
    case '}':
        if (top_is(C_INTERPOL)) {
            if (LEX_CTX[csp - 1].braces == 0) { pop_ctx(); return TK_INTERPOL_END; }
            --LEX_CTX[csp - 1].braces;
        }
        return TK_R_BRACE;
    case '[': return TK_L_BRACK;
    case ']': return TK_R_BRACK;
    case '@': return TK_AT;
    case ':': return TK_COLON;
    case ',': return TK_COMMA;
    case '.':
        if (is_digit(cur())) { consume_digits(); return consume_scientific(); }
        return TK_DOT;
    case '?': return TK_QUESTION;
    case ';': return TK_SEMICOLON;
    case '(': return TK_L_PAREN;
    case ')': return TK_R_PAREN;
    case '+':
        if (cur() == '+') { next(); return TK_CONCAT; }
        return TK_ADD;
    case '-':
        if (cur() == '>') { next(); return TK_IMPLICATION; }
        return TK_SUB;
    case '/':
        if (cur() == '/') { next(); return TK_UPDATE; }
        return TK_DIV;
    case '*': return TK_MUL;
    case '<':
        if (cur() == '|') { next(); return TK_PIPE_LEFT; }
        if (kind == ID_PATH_SEARCH) {
            n = off;
            consume_path();
            if (off == n) return TK_LESS;                  /* <> is not a search path */
            if (next() != '>') return TK_ERROR;
            return TK_PATH_SEARCH;
        }
        if (cur() == '=') { next(); return TK_LESS_OR_EQ; }
        return TK_LESS;
    case '&':
        if (cur() == '&') { next(); return TK_AND_AND; }
        return TK_ERROR;
    case '|':
        if (cur() == '>') { next(); return TK_PIPE_RIGHT; }
        if (cur() == '|') { next(); return TK_OR_OR; }
        return TK_ERROR;
    case '>':
        if (cur() == '=') { next(); return TK_MORE_OR_EQ; }
        return TK_MORE;
    case '$':
        if (cur() == '{') { next(); push_ctx(C_INTERPOL, 0); return TK_INTERPOL_START; }
        return TK_ERROR;
    case '"':
        push_ctx(C_STRING_BODY, 0);
        return TK_STRING_START;
    case '\'':
        if (cur() == '\'') { next(); push_ctx(C_STRING_BODY, 1); return TK_STRING_START; }
        return TK_ERROR;
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
        if (kind != ID_URI) kind = ID_IDENT;               /* a search path that did not start with < */
        for (;;) {
            c = cur();
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c) || c == '_' || c == '-' ||
                c == '\'' || (kind == ID_URI && is_valid_uri_char(c)))
                ++off;
            else
                break;
        }
        return kind == ID_URI ? TK_URI : keyword(start);
    }
    if (is_digit(c)) {
        /* Nix: a 0 followed by digits is an INT whatever follows (rnix #157) */
        if (c == '0' && cur() != '.') { consume_digits(); return TK_INTEGER; }
        consume_digits();
        if (cur() == '.') { next(); consume_digits(); return consume_scientific(); }
        return TK_INTEGER;
    }
    return TK_ERROR;
}

/* --- the token stream ------------------------------------------------------------- */
/* UTF-8, as Rust's String::from_utf8 has it: no overlong form, no
 * surrogate, nothing past U+10FFFF */
int utf8_ok(const uint8_t *text, uint16_t n)
{
    uint16_t i = 0, k, len;
    uint8_t b;
    while (i < n) {
        b = text[i];
        if (b < 0x80) { ++i; continue; }
        if (b < 0xC2 || b > 0xF4) return 0;
        len = ulen(b);
        if ((uint16_t)(n - i) < len) return 0;
        for (k = 1; k < len; ++k)
            if ((text[i + k] & 0xC0) != 0x80) return 0;
        if (b == 0xE0 && text[i + 1] < 0xA0) return 0;
        if (b == 0xED && text[i + 1] > 0x9F) return 0;
        if (b == 0xF0 && text[i + 1] < 0x90) return 0;
        if (b == 0xF4 && text[i + 1] > 0x8F) return 0;
        i += len;
    }
    return 1;
}

/* The text being lexed: the parser reads a token's bytes from it */
const uint8_t *lex_text(void) { return src; }

void lex_init(const uint8_t *text, uint16_t n)
{
    if (!utf8_ok(text, n)) err_exit(E_UNSUP);       /* nbc's read_to_string refuses it */
    src = text;
    slen = n;
    off = 0;
    csp = 0;
}

uint8_t lex_next(uint16_t *start)
{
    *start = off;
    return next_inner();
}

void lex_dump(void)
{
    uint16_t n = *(volatile uint16_t *)SRC_LEN_ADDR, start, count = 0;
    uint8_t kind, *out = (uint8_t *)FRONT_OUT;
    lex_init((const uint8_t *)SRC_BASE, n);
    for (;;) {
        kind = lex_next(&start);
        if (kind == TK_EOF) break;
        if (count >= 0x5555U) nomem();                     /* three bytes a token, one bank */
        out[0] = kind;
        out[1] = (uint8_t)(off - start);
        out[2] = (uint8_t)((uint16_t)(off - start) >> 8);
        out += 3;
        ++count;
    }
    *(uint16_t *)RESULT_ADDR = count;
}
