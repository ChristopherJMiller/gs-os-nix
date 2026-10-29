/* front.h -- the on-target compiler's front end (docs/COMPILER.md, D33).
 *
 * A port of nbc: rnix 0.14.0's tokenizer (lex.c), then its grammar and
 * nbc's lowering, then nbc's compiler. Each stage is diffed against the
 * host's same stage (scripts/front-diff.py).
 */
#ifndef FRONT_H
#define FRONT_H

/* rnix's SyntaxKind tokens, in its order (kinds.rs), so that a token's number
 * is rnix's discriminant: `nbc --tokens` prints the same numbers. */
enum tok {
    TK_ADD, TK_AND_AND, TK_ASSIGN, TK_ASSERT, TK_AT, TK_COLON, TK_COMMA, TK_COMMENT,
    TK_CONCAT, TK_CUR_POS, TK_DIV, TK_DOT, TK_ELLIPSIS, TK_ELSE, TK_EQUAL, TK_ERROR,
    TK_FLOAT, TK_IDENT, TK_IF, TK_IMPLICATION, TK_IN, TK_INHERIT, TK_INTEGER,
    TK_INTERPOL_END, TK_INTERPOL_START, TK_INVERT, TK_L_BRACE, TK_L_BRACK, TK_L_PAREN,
    TK_LESS, TK_LESS_OR_EQ, TK_LET, TK_MORE, TK_MORE_OR_EQ, TK_MUL, TK_NOT_EQUAL, TK_OR,
    TK_OR_OR, TK_PATH_ABS, TK_PATH_HOME, TK_PATH_REL, TK_PATH_SEARCH, TK_PIPE_LEFT,
    TK_PIPE_RIGHT, TK_QUESTION, TK_R_BRACE, TK_R_BRACK, TK_R_PAREN, TK_REC, TK_SEMICOLON,
    TK_STRING_CONTENT, TK_STRING_END, TK_STRING_START, TK_SUB, TK_THEN, TK_UPDATE, TK_URI,
    TK_WHITESPACE, TK_WITH,
    TK_EOF = 0xFF                       /* not rnix's: the end, where its iterator stops */
};

/* and its node kinds, after the tokens */
enum node {
    NODE_APPLY = TK_WITH + 1, NODE_ASSERT, NODE_ATTR_SET, NODE_ATTRPATH, NODE_ATTRPATH_VALUE,
    NODE_BIN_OP, NODE_CUR_POS, NODE_DYNAMIC, NODE_ERROR, NODE_HAS_ATTR, NODE_IDENT,
    NODE_IDENT_PARAM, NODE_IF_ELSE, NODE_INHERIT, NODE_INHERIT_FROM, NODE_INTERPOL, NODE_LAMBDA,
    NODE_LEGACY_LET, NODE_LET_IN, NODE_LIST, NODE_LITERAL, NODE_PAREN, NODE_PAT_BIND,
    NODE_PAT_ENTRY, NODE_PATTERN, NODE_PATH_ABS, NODE_PATH_HOME, NODE_PATH_REL,
    NODE_PATH_SEARCH, NODE_ROOT, NODE_SELECT, NODE_STRING, NODE_UNARY_OP, NODE_WITH
};
#define NODE_FIRST NODE_APPLY

/* A tree element: a token (a = its start, b = its length) or a node (a =
 * its first child in the store, b = how many). Six bytes. The finished
 * children are in bank 15. */
struct el { uint8_t kind, pad; uint16_t a, b; };
#define CST_STORE ((struct el *)FRONT_OUT)

/* The compiled file's GS/OS pathname -- its directory resolves its imports,
 * its last component is __curPos's file -- or just its name: a2min writes it
 * at the top of the source bank, NUL-terminated (a source is at most $FF00
 * bytes, the imports' after it). */
#define SRC_NAME  0x000DFF00UL

/* The directory the fixtures' paths and positions name (front.rs FIXTURE_PWD) */
#define FIXTURE_PWD "/pwd/lang"

/* The compiler's own bank (bank 12; no code lands there either): scratch for
 * a string or a path being built, a canonical path, and the import chain. */
#define FRONT_SCRATCH 0x000C0000UL
#define FRONT_CANON   0x000C8000UL
#define FRONT_CHAIN   0x000CC000UL

/* gsos.c: an import's file at compile time; 0 for the primop */
int gsos_import(const uint8_t *p, uint16_t n, uint8_t *dst, uint16_t max, uint16_t *len);
const uint8_t *gsos_res(uint16_t *n);

/* The lowered AST, as nbc's ast.rs (lower.c). A string: its length, then its
 * bytes. A list cell: the next, two payloads, a position. A node: its tag,
 * an operator or flags, a position, three payloads -- an X_INT's or an
 * X_FLOAT's 8 bytes are a and b. */
struct xs { uint16_t len; uint8_t text[2]; };
#define XS_TEXT(s) ((uint8_t *)(s) + 2)
struct xc { struct xc *next; void *a, *b; uint16_t line, col; };
struct xn { uint8_t tag, op; uint16_t line, col; void *a, *b, *c; };
enum xtag {
    X_INT, X_FLOAT, X_VAR, X_NEG, X_NOT, X_BIN, X_AND, X_OR, X_IMPLIES, X_IF, X_LET, X_LAMBDA,
    X_WITH, X_APPLY, X_ASSERT, X_STR, X_PATHI, X_PATH, X_LIST, X_ATTRS, X_DYNATTRS, X_RECSET,
    X_SELECT, X_HASATTR, X_UPDATE, X_CONCAT, X_INHERIT, X_SEQ, X_IMPORT
};
enum xbin { B_ADD, B_SUB, B_MUL, B_DIV, B_EQ, B_NE, B_LT, B_LE, B_GT, B_GE };

/* --- lex.c ----------------------------------------------------------------------- */
/* The source is n bytes at `text`, which must be UTF-8 (E_UNSUP otherwise, as
 * nbc refuses it). Each lex_next() is one rnix token: its kind, and it spans
 * from *start to lex_off. */
void lex_init(const uint8_t *text, uint16_t n);
int utf8_ok(const uint8_t *text, uint16_t n);
uint8_t lex_next(uint16_t *start);
const uint8_t *lex_text(void);
extern uint16_t lex_off;

/* --lex (a2min): every token of the source at SRC_BASE, as (kind, length)
 * records at FRONT_OUT, their count at RESULT_ADDR. */
void lex_dump(void);

/* --- parse.c ----------------------------------------------------------------------- */
/* rnix's syntax tree of the source; the first error rnix would report is
 * E_PARSE. Then the root and its descendants: */
void parse_file(const uint8_t *text, uint16_t n);
void parse_file_at(const uint8_t *text, uint16_t n);
const struct el *cst_root(void);
struct cst_mark { uint16_t root, w, s; };
void cst_mark(struct cst_mark *m);
void cst_release(const struct cst_mark *m);
const struct el *cst_child(const struct el *node, uint16_t i);
/* --cst (a2min): the tree in preorder, as nbc --cst prints rnix's */
void cst_dump(void);

/* --- lower.c ----------------------------------------------------------------------- */
/* front.rs over the tree parse_file built: the AST; what nbc refuses, E_UNSUP */
struct xn *lower_file(const uint8_t *text, uint16_t n);
/* --ast (a2min): the AST as nbc --ast prints it, an S-expression */
void ast_dump(void);
/* for compile.c: the arena after the AST, and a text with no file lowered */
void *front_alloc(uint16_t n);
struct xn *lower_text(const uint8_t *text, uint16_t n);

/* --- compile.c ---------------------------------------------------------------------- */
/* compiler.rs and nbc.rs write_program over the AST: the image nbc writes,
 * at IMAGE_BASE; its length */
uint16_t compile_file(const struct xn *program);
/* --compile (a2min): the source at SRC_BASE compiled, the image's length at
 * RESULT_ADDR */
void compile_dump(void);
/* --run-nix (a2min): the source at SRC_BASE compiled, for the VM to run */
void compile_run(void);
/* the n bytes at SRC_BASE compiled into IMAGE_BASE: the image's length */
uint16_t compile_source(uint16_t n);

/* --- repl.c ------------------------------------------------------------------------ */
/* The line read, for front_run to compile: its length (the line is at
 * SRC_BASE), beside the compiler's and the printer's registers */
#define REPL_LEN (*(uint16_t *)(FRONT_CSTATE + 0xF0UL))

/* The compiler's registers: the top of bank 12, above the import chain,
 * so they cost bank 0 nothing */
#define FRONT_CSTATE  0x000CFF00UL
#define FRONT_CHAIN_MAX 0x3EFEU                 /* the chain's bytes, below FRONT_CSTATE */

/* The stage FRONT_MODE_ADDR names (vm_entry's one test of it: the dispatch
 * is here, in "front", so that a stage costs bank 0 nothing): 1 if it left
 * an image at IMAGE_BASE for the VM to run, 0 if its output is the result */
int front_run(void);

#endif
