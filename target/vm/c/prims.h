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
             *str_path, *str_tostring, *str_outpath, *str_type, *str_float;

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
void   str_sha256(obj_t *so, uint8_t *dig);         /* of a forced STRING original, on the card if it can */

/* Building contexts: an element STRING of [pre] ["!<o>!"] s[off..off+n)
 * (n = CTX_REST: to the end), and the sorted set of them on the stack. */
#define CTX_REST 0xFFFF
obj_t *ctx_piece(char pre, obj_t *o, obj_t *s, uint16_t off, uint16_t n);   /* may collect */
int    ctx_insert(uint16_t base, obj_t *r);         /* into vstack[base..vsp), sorted, once */

void prim_stringlength(void);
void prim_substring(void);
void prim_tostring(void);
void prim_hashstring(void);
void prim_concatstringssep(void);
void prim_basenameof(void);
void prim_dirof(void);
void prim_hascontext(void);
void prim_compareversions(void);                    /* strings.c: names.cc's rules */
void prim_splitversion(void);
void prim_parsedrvname(void);
void prim_replacestrings(void);
void prim_tojson(void);                             /* json.c */
void prim_fromjson(void);
void prim_toxml(void);
void prim_topath(void);                             /* strings.c */
void prim_match(void);                              /* match.c */
void prim_split(void);                              /* match.c */
_Noreturn void prim_gsos(uint16_t id, uint16_t arity);   /* strings.c: E_GSOS (D26) */
void prim_findfile(void);                           /* strings.c */
_Noreturn void prim_import(uint16_t scoped);        /* strings.c: E_GSOS (D26) */
void copy_entry(obj_t *dst, uint16_t n, uint16_t i, obj_t *src, uint16_t ns, uint16_t j);   /* prims.c */
obj_t *lit_attr(obj_t *set, const char *lit, uint16_t n);   /* the value a C literal names in set, or 0; no bytecode */
void prim_unsafediscardstringcontext(void);
void prim_unsafediscardoutputdependency(void);
void prim_adddrvoutputdependencies(void);
void prim_getcontext(void);
void prim_appendcontext(void);

/* --- derivation.c ---------------------------------------------------------------------- */
void prim_derivationstrict(void);
void check_name(const char *p, uint16_t n);         /* a store-path name, else E_TYPE */
void prim_tofile(void);

#endif
