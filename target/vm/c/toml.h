/* toml.h -- TOML 1.0 as Nix 2.34's builtins.fromTOML parses it: toml11
 * 4.4.0's parser, ported (toml.c, docs/TOML.md). No VM types: toml.c builds
 * on its own, on the host as on the 65816. */
#ifndef TOML_H
#define TOML_H

#include <stdint.h>

/* Bank 7, the regex engine's too (VM.md): a primop's own until it returns.
 * fromTOML runs no bytecode between parsing and building its value. */
#define TOML_BASE   0x00070000UL

#define TM_EINVAL   1   /* not TOML, or a date or time: Nix's "while parsing TOML" */
#define TM_ESPACE   2   /* the bank is full, or nesting too deep: E_NOMEM */
_Noreturn void tm_fail(uint8_t why);    /* the caller's; does not return */

/* toml::parse of s: the root table's node; tm_fail unless s is TOML 1.0
 * that Nix accepts (no dates or times). */
uint16_t tm_parse(const char *s, uint16_t n);

/* The tree, in bank 7. A node is a table, an array, a string, an integer, a
 * float or a boolean; a table's entries and an array's elements are a list. */
#define TM_TABLE    'T'
#define TM_ARRAY    'A'
#define TM_STRING   'S'
#define TM_INT      'I'
#define TM_FLOAT    'F'
#define TM_BOOL     'B'
uint8_t  tm_kind(uint16_t node);
uint16_t tm_first(uint16_t node);       /* a table's first entry, an array's first element; 0 at the end */
uint16_t tm_next(uint16_t item);
uint16_t tm_val(uint16_t item);
uint16_t tm_key(uint16_t entry);        /* a STRING node */
uint16_t tm_len(uint16_t node);         /* a string's bytes */
const uint8_t *tm_bytes(uint16_t node); /* a string's bytes; an integer's or a float's 8, little-endian */
uint8_t  tm_bool(uint16_t node);

/* A table's entries in Nix's order, its keys' bytes */
void tm_sort(uint16_t table);

#endif
