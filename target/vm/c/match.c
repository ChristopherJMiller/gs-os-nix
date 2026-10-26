/* match.c -- builtins.match and builtins.split on regex.c (docs/REGEX.md):
 * Nix's prim_match and prim_split, the values they build from the matches. */
#pragma noroot
#pragma optimize 15

#include <string.h>
#include "vm.h"
#include "prims.h"
#include "regex.h"

/* A segment of its own: bank 6, with regex.c (VM.md). */
segment "regex";

_Noreturn void rx_fail(uint8_t why)
{
    if (why == RX_ESPACE) nomem();
    err_exit(E_REGEX);
}

/* Nix forces the pattern -- a string without context -- and compiles it
 * before it forces the string, so an invalid pattern is the error even when
 * the string throws. Forcing the string can run a match of its own, which
 * reuses bank 7, so the pattern is compiled again after: the view of the
 * string, which is a STRING (forceString: a path is E_TYPE). */
static obj_t *rx_args(void)
{
    obj_t *r, *s;
    force_slot(2);
    r = deref(vstack[vsp - 2]);
    if (r->tag != T_STRING || CTX(r)) type_err();
    rx_compile(STRBYTES(r), AUX16(r));
    force_slot(1);
    s = deref(vstack[vsp - 1]);
    if (s->tag != T_STRING) type_err();
    r = deref(vstack[vsp - 2]);
    rx_compile(STRBYTES(r), AUX16(r));
    return s;
}

/* The bytes a STRING of n bytes, and a LIST of n elements, take */
#define STR_SIZE(n)  ((uint32_t)round4(n) + 8)
static uint32_t list_size(uint16_t n)
{
    uint32_t t = n;
    if (n == 0) return 8;
    t += t;
    return t + t + 4;
}

/* Bytes f..e of the view s, as a fresh STRING with no context (Nix's
 * mkString of a sub_match). The caller has reserved it. */
static obj_t *piece(obj_t *s, uint16_t f, uint16_t e)
{
    obj_t *o = str_alloc(e - f);
    memcpy(STRBYTES(o), STRBYTES(s) + f, e - f);
    return o;
}

/* Groups 1.. of the record at rec: a LIST of strings, null where a group
 * did not take part. Reserved by the caller. */
static obj_t *groups(obj_t *s, uint16_t rec, uint16_t g)
{
    obj_t *l = list_alloc(g);
    uint16_t k;
    for (k = 1; k <= g; ++k) {
        if (rx_matched(rec, k)) ELEMS(l)[k - 1] = piece(s, rx_first(rec, k), rx_second(rec, k));
        else ELEMS(l)[k - 1] = obj_null;
    }
    return l;
}

static uint32_t groups_size(uint16_t rec, uint16_t g)
{
    uint32_t t = list_size(g);
    uint16_t k;
    for (k = 1; k <= g; ++k)
        if (rx_matched(rec, k)) t += STR_SIZE(rx_second(rec, k) - rx_first(rec, k));
    return t;
}

/* Everything the result needs, at once, so that building it cannot collect:
 * then the view of the string again. */
static obj_t *reserve(uint32_t bytes)
{
    if (bytes > 0xFFFFUL) nomem();
    nursery_reserve((uint16_t)bytes);
    return deref(vstack[vsp - 1]);
}

/* builtins.match re s: null unless all of s matches; else the groups */
void prim_match(void)
{
    obj_t *s = rx_args(), *l;
    uint16_t g, rec;
    if (!rx_match(STRBYTES(s), AUX16(s))) {
        vsp -= 2;
        push(obj_null);
        return;
    }
    g = rx_groups() - 1;
    rec = rx_records();
    s = reserve(groups_size(rec, g));
    l = groups(s, rec, g);
    vsp -= 2;
    push(l);
}

/* builtins.split re s: [ before, groups, between, groups, ..., after ], or
 * [ s ] -- the argument itself, its context kept -- when nothing matches.
 * The pieces have no context. */
void prim_split(void)
{
    obj_t *s = rx_args(), *l;
    uint16_t m, g, rs, rec, prev, i, j;
    uint32_t total;
    m = rx_split(STRBYTES(s), AUX16(s));
    if (m == 0) {
        nursery_reserve(8);
        l = list_alloc(1);
        ELEMS(l)[0] = vstack[vsp - 1];
        vsp -= 2;
        push(l);
        return;
    }
    g = rx_groups() - 1;
    rs = rx_recsize();
    total = list_size(m) + list_size(m);            /* 2m + 1 elements: 8m + 8 bytes */
    prev = 0;
    for (i = 0, rec = rx_records(); i != m; ++i, rec += rs) {
        total += STR_SIZE(rx_first(rec, 0) - prev) + groups_size(rec, g);
        prev = rx_second(rec, 0);
    }
    total += STR_SIZE(AUX16(s) - prev);
    s = reserve(total);
    l = list_alloc(m + m + 1);
    prev = 0;
    for (i = 0, j = 0, rec = rx_records(); i != m; ++i, rec += rs) {
        ELEMS(l)[j++] = piece(s, prev, rx_first(rec, 0));
        ELEMS(l)[j++] = groups(s, rec, g);
        prev = rx_second(rec, 0);
    }
    ELEMS(l)[j] = piece(s, prev, AUX16(s));
    vsp -= 2;
    push(l);
}
