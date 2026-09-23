/* gc.c -- the nursery collector (HEAP.md, "The VM and the two spaces").
 *
 * Runs only between two bytecode instructions (run() calls gc_maybe() before
 * every dispatch), when the live pointers are exactly the roots listed below
 * and no C local holds an object. A Cheney copy of the live graph goes into a
 * STAGING bank at the SAME low addresses the survivors will have afterwards,
 * so pointers written during the copy are already the final ones; then
 *
 *   step C (D6, "the generational boundary is the slot connector"): every
 *           pointer field in the staging area is translated by the constant
 *           card = base + (offset - gc_water), the whole area goes to the card
 *           as ONE DATA burst into a RESERVEd block, and the roots are
 *           rewritten the same way. The nursery is then empty. Survivors cross
 *           the bus once, at about one access per byte; garbage never does.
 *   step B (without a card): the survivors are block-moved back into the
 *           nursery instead -- an in-RAM semispace collection.
 *
 * The constants materialised from the .nbc are roots like any other (the
 * constant tables), so the first collection promotes them and every later one
 * ignores them: no object is pinned in the nursery, and so no mature object
 * can point into it -- HEAP.md's invariant, kept by the write barrier
 * (data.c store_field). The program's own static objects are in bank 0 and
 * are not young at all. Mature objects never move (the card does not compact
 * yet), so the collector never reads the card.
 *
 * Roots: the value stack; env; RSTACK; FSTACK; the env of every catch frame;
 * the constant tables; the imported-file thunks (run.c memoises one per file,
 * so `import ./f == import ./f` is true -- BYTECODE.md "Imports"). Every root
 * is walked on EVERY collection, so each list is bounded by what is in use
 * (cnext, nchunks), not by its capacity: the write barrier can trigger a
 * collection per store, which makes the root scan a per-store cost. Not a pointer: word 0 of a THUNK/LAMBDA/BLACKHOLE (code
 * and constant-table offsets).
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

/* The size in bytes of an object from its header. Marks in aux2 are ignored. */
uint16_t obj_size(const obj_t *o)
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

/* p -> its pointer after the collection. Null, mature and static pointers pass
 * through; nothing is pinned. A young object is copied to to-space once; its old
 * header becomes FORWARDED with the new pointer, so a second reference finds
 * the copy. */
static obj_t *evac(obj_t *p)
{
    uint16_t size, at;
    obj_t *dst;
    if (!IS_YOUNG(p)) return p;
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

/* --- promotion --------------------------------------------------------------------- */
static uint32_t xbase;      /* the RESERVEd block: staging offset gc_water lands here */

/* A young pointer -> its card address. */
static obj_t *translate(obj_t *p)
{
    if (!IS_YOUNG(p)) return p;
    return (obj_t *)(xbase + (uint32_t)(uint16_t)(LOW16(p) - gc_water));
}

/* Rewrite every pointer field of the to-space object at `off` in place, then
 * return its size. */
static uint16_t translate_one(uint16_t off)
{
    obj_t *o = STAGE_AT(off);
    uint16_t n = AUX16(o), i, size = obj_size(o);
    obj_t **f;
    switch (o->tag) {
    case T_LIST:
        f = ELEMS(o);
        for (i = 0; i < n; ++i) f[i] = translate(f[i]);
        break;
    case T_ATTRS:
        f = AVALUES(o, n);
        for (i = 0; i < 2 * n; ++i) f[i] = translate(f[i]);
        break;
    case T_THUNK: case T_LAMBDA: case T_BLACKHOLE:
        CL_ENV(o) = translate(CL_ENV(o));
        break;
    case T_INDIRECT:
        IND_TARGET(o) = translate(IND_TARGET(o));
        break;
    case T_STRING:
        STRCTX(o, n) = translate(STRCTX(o, n));
        break;
    default:
        break;
    }
    return size;
}

void gc_collect(void)
{
    uint16_t i, off, size;
    ++ST_COLLECT;
    gfree = gscan = gc_water;
    for (i = 0; i < vsp; ++i) vstack[i] = evac(vstack[i]);
    env = evac(env);
    for (i = 0; i < rsp; ++i) rstack[i] = evac(rstack[i]);
    for (i = 0; i < fstack_n; ++i) fstack[i] = evac(fstack[i]);
    for (i = 0; i < catch_n; ++i) catches[i].env = evac(catches[i].env);
    for (i = 0; i < cnext; ++i) ctab_pool[i] = evac(ctab_pool[i]);
    for (i = 0; i < nchunks; ++i) imports[i] = evac(imports[i]);
    while (gscan != gfree) scan_one();
    if (gfree == gc_water) { nfree = gc_water; return; }   /* nothing survived */
    if (slot_x == 0) {
        /* step B: no card -- the survivors go back into the nursery */
        memcpy(YOUNG_PTR(gc_water), STAGE_AT(gc_water), (uint16_t)(gfree - gc_water));
        nfree = gfree;
        return;
    }
    /* step C: promote. RESERVE the block, translate every pointer field in the
     * staging area, stream it across in one burst, rewrite the roots. */
    size = (uint16_t)(gfree - gc_water);
    ST_PROMOTED += size;
    xbase = card_reserve(size);
    for (off = gc_water; off != gfree; off += translate_one(off)) ;
    card_write_at(xbase, STAGE_AT(gc_water), size);
    for (i = 0; i < vsp; ++i) vstack[i] = translate(vstack[i]);
    env = translate(env);
    for (i = 0; i < rsp; ++i) rstack[i] = translate(rstack[i]);
    for (i = 0; i < fstack_n; ++i) fstack[i] = translate(fstack[i]);
    for (i = 0; i < catch_n; ++i) catches[i].env = translate(catches[i].env);
    for (i = 0; i < cnext; ++i) ctab_pool[i] = translate(ctab_pool[i]);
    for (i = 0; i < nchunks; ++i) imports[i] = translate(imports[i]);
    nfree = gc_water;                           /* the nursery is empty */
}
