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

/* --- lex.c ----------------------------------------------------------------------- */
/* The source is n bytes at `text`, which must be UTF-8 (E_UNSUP otherwise, as
 * nbc refuses it). Each lex_next() is one rnix token: its kind, and it spans
 * from *start to lex_off. */
void lex_init(const uint8_t *text, uint16_t n);
uint8_t lex_next(uint16_t *start);
extern uint16_t lex_off;

/* --lex (a2min): every token of the source at SRC_BASE, as (kind, length)
 * records at FRONT_OUT, their count at RESULT_ADDR. */
void lex_dump(void);

#endif
