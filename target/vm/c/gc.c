/* gc.c -- the nursery collector (HEAP.md, "The VM and the two spaces").
 *
 * Runs only between two bytecode instructions (run() calls gc_maybe() before
 * every dispatch), when the live pointers are exactly the roots listed below
 * and no C local holds an object. A Cheney copy of the live graph goes into a
 * STAGING bank at the SAME low addresses the survivors will have afterwards,
 * so pointers written during the copy are already the final ones; then
 *
 *   step B (today): the compacted survivors are block-moved back into the
 *           nursery -- an in-RAM semispace collection;
 *   step C: the staging area is RESERVEd and streamed to the card instead,
 *           and the roots are rewritten to mature addresses (promotion, D6;
 *           target/vm/promote.s is the assembly draft).
 *
 * Constants materialised from the .nbc sit below gc_water and are pinned:
 * never moved, never scanned (they hold no pointers). The program's own
 * static objects are in bank 0 and are not young at all.
 *
 * Roots: the value stack; env; RSTACK; FSTACK; the env of every catch frame.
 * Not a pointer: word 0 of a THUNK/LAMBDA/BLACKHOLE (code and constant-table
 * offsets).
 */
#pragma noroot
#pragma optimize 15

#include "vm.h"

static uint16_t gfree;      /* to-space bump pointer (offset; bank STAGE) */
static uint16_t gscan;      /* Cheney scan pointer */

#define STAGE_AT(off) ((obj_t *)(YOUNG_TAG | STAGE_BASE | (uint32_t)(uint16_t)(off)))

void gc_maybe(void)
{
    if (*(volatile uint8_t *)GC_STRESS_ADDR) {
        if (nfree != gc_water) gc_collect();
    } else if (nfree >= NURSERY_END - GC_MARGIN) {
        gc_collect();
    }
}

/* The size in bytes of the young object at o. Marks in aux2 are ignored. */
static uint16_t obj_size(obj_t *o)
{
    uint16_t n = AUX16(o);
    switch (o->tag) {
    case T_INT: case T_THUNK: case T_LAMBDA: case T_BLACKHOLE: return 12;
    case T_STRING: return round4(n) + 8;
    case T_LIST:   return 4 + 4 * n;
    case T_ATTRS:  return 4 + 12 * n;
    default:       return 8;                    /* BOOL, NULL, INDIRECT */
    }
}

/* p -> its pointer after the collection. Null, mature, static and pinned
 * pointers pass through. A young object is copied to to-space once; its old
 * header becomes FORWARDED with the new pointer, so a second reference finds
 * the copy. */
static obj_t *evac(obj_t *p)
{
    uint16_t size, at;
    obj_t *dst;
    if (!IS_YOUNG(p)) return p;
    if (LOW16(p) < gc_water) return p;          /* a pinned constant */
    if (p->tag == T_FORWARDED) return IND_TARGET(p);
    size = obj_size(p);
    at = gfree;
    dst = STAGE_AT(at);
    memcpy(dst, p, size);
    gfree += size;
    p->tag = T_FORWARDED;
    IND_TARGET(p) = YOUNG_PTR(at);
    return YOUNG_PTR(at);
}

/* The to-space object at gscan: evacuate every pointer field, then advance. */
static void scan_one(void)
{
    obj_t *o = STAGE_AT(gscan);
    uint16_t n = AUX16(o), i;
    obj_t **f;
    switch (o->tag) {
    case T_LIST:
        f = ELEMS(o);
        for (i = 0; i < n; ++i) f[i] = evac(f[i]);
        gscan += 4 + 4 * n;
        break;
    case T_ATTRS:
        f = AVALUES(o, n);                      /* values then names: 2n pointers */
        for (i = 0; i < 2 * n; ++i) f[i] = evac(f[i]);
        gscan += 4 + 12 * n;
        break;
    case T_THUNK: case T_LAMBDA: case T_BLACKHOLE:
        CL_ENV(o) = evac(CL_ENV(o));
        gscan += 12;
        break;
    case T_INDIRECT:
        IND_TARGET(o) = evac(IND_TARGET(o));
        gscan += 8;
        break;
    case T_STRING:
        STRCTX(o, n) = evac(STRCTX(o, n));
        gscan += round4(n) + 8;
        break;
    case T_INT:
        gscan += 12;
        break;
    default:
        gscan += 8;
    }
}

void gc_collect(void)
{
    uint16_t i;
    gfree = gscan = gc_water;
    for (i = 0; i < vsp; ++i) vstack[i] = evac(vstack[i]);
    env = evac(env);
    for (i = 0; i < rsp; ++i) rstack[i] = evac(rstack[i]);
    for (i = 0; i < fstack_n; ++i) fstack[i] = evac(fstack[i]);
    for (i = 0; i < catch_n; ++i) catches[i].env = evac(catches[i].env);
    while (gscan != gfree) scan_one();
    /* step B: the survivors go back into the nursery */
    if (gfree != gc_water)
        memcpy(YOUNG_PTR(gc_water), STAGE_AT(gc_water), (uint16_t)(gfree - gc_water));
    nfree = gfree;
}
