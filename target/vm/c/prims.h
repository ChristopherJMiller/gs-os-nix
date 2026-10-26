/* prims.h -- what the three primop files share: prims.c (the shared helpers,
 * lists, sets, errors and the table), strings.c (strings, coercion, string
 * context) and derivation.c (derivationStrict). Private to them: vm.h is the
 * VM's interface. All three are `segment "prims"`, one load segment above
 * bank 0 (VM.md); sharing a function costs nothing there, but every static,
 * the strings below included, is bank 0 whichever file defines it.
 */
#ifndef PRIMS_H
#define PRIMS_H

#include "vm.h"

/* --- prims.c --------------------------------------------------------------------------- */
/* The VM's static strings: immutable STRINGs it hands out instead of allocating. */
extern obj_t *str_int, *str_bool, *str_null, *str_string, *str_list, *str_set, *str_lambda,
             *str_one, *str_dot, *str_slash, *str_name, *str_right, *str_wrong, *str_space,
             *str_path, *str_tostring, *str_outpath, *str_type;

/* A STRING's context (DERIVATION.md "String context"): 0, or a LIST of STRINGs
 * sorted by bytes, no two equal, in Nix's element syntax -- "!<output>!<drv>",
 * "=<drv>", "/nix/store/...". s is a STRING view. */
#define CTX(s) STRCTX((s), AUX16(s))

/* The i-th element of the LIST (of n elements) whose pointer is root stack
 * entry r. Re-fetched every time round, because the loop calls back into Nix
 * (which may collect or turn the window ring over); list_elem makes that cheap
 * for a long mature list by reading one element rather than copying it. */
#define RELEM(r, i, n) list_elem(rstack[r], (i), (n))

obj_t *force_pop_as(uint8_t tag, obj_t **orig);    /* force, pop, type-check: the view; *orig the original */
obj_t *list_from_stack(uint16_t n);                 /* the n values on top become a fresh LIST */
obj_t *view_top_text(void);                         /* the top forced in place, a STRING or a PATH: a view */
void   insert_entry(obj_t *set, uint16_t cap, uint16_t *cnt, obj_t *name, obj_t *value);

/* --- strings.c ------------------------------------------------------------------------- */
int    is_lit(obj_t *s, const char *lit, uint16_t n);  /* the STRING view s is lit[0..n) */
#define IS_LIT(s, lit) is_lit((s), (lit), sizeof(lit) - 1)
void   hex_of(const uint8_t *dig, uint16_t n, char *out);  /* n bytes as 2n lowercase hex digits */
obj_t *ctx_join(uint16_t at, uint16_t n);           /* the union of vstack[at..at+n)'s contexts; may collect */

void prim_stringlength(void);
void prim_substring(void);
void prim_tostring(void);
void prim_hashstring(void);
void prim_concatstringssep(void);
void prim_basenameof(void);
void prim_dirof(void);

/* --- derivation.c ---------------------------------------------------------------------- */
void prim_derivationstrict(void);

#endif
