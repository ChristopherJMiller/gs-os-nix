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
 * ignores them: no object is pinned in the nursery, and so a mature object
 * points into it only through a field the write barrier recorded (data.c
 * store_field, the remembered set below; D20). The program's own static
 * objects are in bank 0 and are not young at all. Mature objects never move
 * (the card does not compact yet), so the only card reads here are the
 * recorded fields.
 *
 * Roots: the value stack; env; RSTACK; FSTACK; the env of every catch frame;
 * the constant tables; the imported-file thunks (run.c memoises one per file,
 * so `import ./f == import ./f` is true -- BYTECODE.md "Imports"); the
 * remembered fields. Every root is walked on EVERY collection, so each list is
 * bounded by what is in use (cnext, nchunks, rs_n), not by its capacity. Not a
 * pointer: word 0 of a THUNK/LAMBDA/BLACKHOLE (code and constant-table offsets).
 */
#pragma noroot
#pragma optimize 15

#include "vm.h"

static uint16_t gfree;      /* to-space bump pointer (offset; bank STAGE) */
static uint16_t gscan;      /* Cheney scan pointer */

#define STAGE_AT(off) ((obj_t *)(YOUNG_TAG | STAGE_BASE | (uint32_t)(uint16_t)(off)))

/* --- the remembered set -------------------------------------------------------------- */
/* Every mature field the write barrier stored a young pointer into, as (object,
 * offset). The collection treats each as a root: it reads the field back from
 * the card, evacuates what it points at, and after promotion writes the card
 * address over it. So a store into a mature object costs O(1) and the only
 * collections are for memory pressure (HEAP.md). `val` is scratch for the
 * collection: the field's staging pointer between evacuation and write-back.
 *
 * Between collections this is the one place a mature object points into the
 * nursery, which is sound only because nothing card-side traces: the card's own
 * MARK must treat these fields as roots, or never run while the set is
 * non-empty (HEAP.md, CARD-ISA.md). Entries are not deduplicated -- a field
 * stored twice is two entries, and evac() is idempotent -- because a search per
 * store would cost more than the rare duplicate does. */
struct rs_entry { obj_t *obj; uint16_t off; obj_t *val; };
#define RSET ((struct rs_entry *)(YOUNG_TAG | RSET_BASE))
uint16_t rs_n;

void remember(obj_t *c, uint16_t off)
{
    struct rs_entry *e;
    /* The caller collects when the set is full; one that forgot would write
     * past the set into bank 5's tail and lose a root in silence -- so loud. */
    if (rs_full()) badnbc();
    e = &RSET[rs_n++];
    e->obj = c;
    e->off = off;
    ++ST_REMEMBERED;
}

/* Is `off` a pointer field of an object with this header NOW? A recorded field
 * can stop being one: a thunk updated with a THUNK value (its environment
 * recorded at offset 8) is forced again and overwritten with an INT, whose high
 * word is then at offset 8 -- and a negative int can look exactly like a young
 * pointer. So the header decides, per HEAP.md's trace table, not the record. */
static int rs_is_ptr(const obj_t *h, uint16_t off)
{
    uint16_t n = AUX16(h);
    switch (h->tag) {
    case T_LIST:  return off >= 4 && off < 4 + 4 * n;
    case T_ATTRS: return off >= 4 + 4 * n && off < 4 + 12 * n;
    case T_THUNK: case T_LAMBDA: case T_BLACKHOLE: return off == 8;
    case T_INDIRECT: return off == 4;
    case T_STRING: return off == 4 + round4(n);
    default: return 0;
    }
}

uint8_t gc_tick;            /* boundaries since the last stress collection */

void gc_maybe(void)
{
    if (gc_due()) gc_collect();
}

/* The size in bytes of an object from its header. Marks in aux2 are ignored.
 *
 * Never less than 8 (HEAP.md "Alignment and minimum size"): evac() writes the
 * forwarding pointer at +4, so a 4-byte object -- an EMPTY list or set, as the
 * C port first allocated them -- had it written over the NEXT object's header.
 * That answered `{ } == { }` false, silently, whenever a collection landed at
 * the wrong moment: GC stress at every 2nd, 5th or 7th boundary found it, and
 * at every boundary did not (eval-okay-deep-equality). */
uint16_t obj_size(const obj_t *o)
{
    uint16_t n = AUX16(o);
    switch (o->tag) {
    case T_INT: case T_THUNK: case T_LAMBDA: case T_BLACKHOLE: return 12;
    case T_STRING: return round4(n) + 8;
    case T_LIST:   return n ? 4 + 4 * n : 8;
    case T_ATTRS:  return n ? 4 + 12 * n : 8;
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

/* The to-space object at gscan: evacuate every pointer field, then advance by
 * obj_size -- the one definition of an object's size, so the scan can never
 * step differently from the copy that laid the object down. */
static void scan_one(void)
{
    obj_t *o = STAGE_AT(gscan);
    uint16_t n = AUX16(o), i, size = obj_size(o);
    obj_t **f;
    switch (o->tag) {
    case T_LIST:
        f = ELEMS(o);
        for (i = 0; i < n; ++i) f[i] = evac(f[i]);
        break;
    case T_ATTRS:
        f = AVALUES(o, n);                      /* values then names: 2n pointers */
        for (i = 0; i < 2 * n; ++i) f[i] = evac(f[i]);
        break;
    case T_THUNK: case T_LAMBDA: case T_BLACKHOLE:
        CL_ENV(o) = evac(CL_ENV(o));
        break;
    case T_INDIRECT:
        IND_TARGET(o) = evac(IND_TARGET(o));
        break;
    case T_STRING:
        STRCTX(o, n) = evac(STRCTX(o, n));
        break;
    default:
        break;
    }
    gscan += size;
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
    for (i = 0; i < rs_n; ++i) {                /* the remembered fields, read back */
        struct rs_entry *e = &RSET[i];
        obj_t hdr, *p = 0;
        card_read_at(PBITS(e->obj), &hdr, 4);
        if (rs_is_ptr(&hdr, e->off)) card_read_at(PBITS(e->obj) + e->off, &p, 4);
        e->val = IS_YOUNG(p) ? evac(p) : 0;
    }
    while (gscan != gfree) scan_one();
    if (gfree == gc_water) { nfree = gc_water; rs_n = 0; return; }   /* nothing survived */
    if (slot_x == 0) {
        /* step B: no card -- the survivors go back into the nursery. Nothing
         * is mature, so the remembered set is empty. */
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
    for (i = 0; i < rs_n; ++i) {                /* and the remembered fields, on the card */
        struct rs_entry *e = &RSET[i];
        obj_t *t;
        if (e->val == 0) continue;
        t = translate(e->val);
        card_write_at(PBITS(e->obj) + e->off, &t, 4);
        windows_invalidate(e->obj);             /* a cached copy still has the young pointer */
    }
    rs_n = 0;
    nfree = gc_water;                           /* the nursery is empty */
}
