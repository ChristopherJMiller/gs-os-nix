/* data.c -- allocation, the stacks, strings, lists, attribute sets, equality
 * and deep force. Translated from target/vm/data.s and equal.s.
 */
#pragma noroot
#pragma optimize 1

#include "vm.h"

obj_t *vstack[VSTACK_N];
uint16_t vsp;
obj_t *rstack[RSTACK_N];
uint16_t rsp;
uint16_t nfree, gc_water;

/* --- the nursery: bump allocation ---------------------------------------------------- */
obj_t *alloc(uint16_t size)
{
    obj_t *o;
    if (size > (uint16_t)(NURSERY_END - nfree)) nomem();
    o = YOUNG_PTR(nfree);
    nfree += size;
    return o;
}

obj_t *new_int(int64_t v)
{
    obj_t *o = alloc(12);
    SET_HDR(o, T_INT, 0);
    INTVAL(o) = v;
    return o;
}

obj_t *str_alloc(uint16_t len)
{
    uint16_t r = round4(len);
    obj_t *o;
    if (r < len) nomem();                       /* wrapped: too long for a bank */
    o = alloc(r + 8);
    SET_HDR(o, T_STRING, len);
    if (r) *(uint32_t *)(PAYLOAD(o) + r - 4) = 0;   /* the pad bytes are zero */
    STRCTX(o, len) = 0;
    return o;
}

obj_t *list_alloc(uint16_t n)
{
    obj_t *o = alloc(4 + 4 * n);
    SET_HDR(o, T_LIST, n);
    return o;
}

obj_t *attrs_alloc(uint16_t n)
{
    obj_t *o = alloc(4 + 12 * n);
    SET_HDR(o, T_ATTRS, n);
    return o;
}

/* --- mature objects: the copy windows ----------------------------------------------- */
/* A mature object is read by copying it through the bus into bank 5, which is
 * used as a ring: copies are laid down one after another and dropped wholesale
 * when the ring turns over. The turnover happens at a BYTECODE BOUNDARY and
 * nowhere else, because a boundary is the one moment no view is live (the same
 * fact the collector relies on); so a view is valid for the whole instruction
 * that took it, and never longer. Within an instruction the ring only bumps
 * forward, and running out is E_NOMEM rather than a wrap onto a live view.
 * A direct-mapped table of recent copies makes the second deref of the same
 * object free -- environments and closure bodies are read again and again, and
 * the table survives until the next turnover.
 *
 * Copying the WHOLE object, rather than a fixed 256-byte window over it, is
 * what keeps every accessor written for young objects valid on the copy --
 * including the three columns of a large set. WINDOW_MAX is the price: an
 * object too big to view at all is E_NOMEM. */
#define WIN_PTR(off) ((obj_t *)(YOUNG_TAG | WINDOW_BASE | (uint32_t)(uint16_t)(off)))
#define NWIN 64
static struct { obj_t *card; uint16_t off; } wins[NWIN];
uint16_t win_ring;              /* the bump pointer; the dispatch loop reads it */

/* Drop every copy and start the ring again. Only ever called at a bytecode
 * boundary (windows_maybe in run()), where no view is live -- which is what
 * makes reusing the bytes safe. */
void windows_turnover(void)
{
    ++ST_TURNOVER;
    memset(wins, 0, sizeof wins);
    win_ring = 0;
}

static uint16_t win_slot(obj_t *p)
{
    uint32_t a = PBITS(p);
    return (uint16_t)((a >> 2) ^ (a >> 8)) & (NWIN - 1);
}

void windows_invalidate(obj_t *p)
{
    uint16_t h = win_slot(p);
    if (wins[h].card == p) wins[h].card = 0;
}

static obj_t *mature_view(obj_t *p)
{
    uint16_t h = win_slot(p), size;
    obj_t hdr, *w;
    if (wins[h].card == p) return WIN_PTR(wins[h].off);
    card_read_at(PBITS(p), &hdr, 4);
    size = obj_size(&hdr);
    if (size > WINDOW_MAX) nomem();
    /* The ring never wraps here: it is turned over at bytecode boundaries only,
     * which leaves every instruction WINDOW_RING - WINDOW_FLOOR to spend. An
     * instruction that wants more is E_NOMEM -- loud -- rather than wrapping
     * onto a view it is still holding, which would answer wrongly in silence. */
    if ((uint16_t)(win_ring + size) > WINDOW_RING) nomem();
    ++ST_VIEWS;
    ST_VIEW_BYTES += size;
    w = WIN_PTR(win_ring);
    card_read_at(PBITS(p), w, size);
    wins[h].card = p;
    wins[h].off = win_ring;
    win_ring += size;                           /* obj_size is always a multiple of 4 */
    return w;
}

/* --- pointers and stacks ---------------------------------------------------------------- */
obj_t *deref(obj_t *p)
{
    if (p == 0) badnbc();
    if (IS_RAM(p)) return p;
    return mature_view(p);
}

/* The write barrier: a store into a mature object promotes the value first --
 * a collection, with the value on the stack as a root -- and goes through the
 * bus. Into a young object it is a plain store. `c` is the object, not a view. */
void store_field(obj_t *c, uint16_t off, obj_t *v)
{
    if (IS_RAM(c)) { *(obj_t **)((uint8_t *)c + off) = v; return; }
    if (IS_YOUNG(v)) { ++ST_BARRIER; push(v); gc_collect(); v = pop(); }
    card_write_at(PBITS(c) + off, &v, 4);
    windows_invalidate(c);
}

void store_byte(obj_t *c, uint16_t off, uint8_t b)
{
    if (IS_RAM(c)) { ((uint8_t *)c)[off] = b; return; }
    card_write_at(PBITS(c) + off, &b, 1);
    windows_invalidate(c);
}

obj_t *force_pop_view(obj_t **view)
{
    obj_t *o;
    force_top();
    o = pop();
    *view = deref(o);
    return o;
}

obj_t *deref_follow(obj_t *p)
{
    p = deref(p);
    while (p->tag == T_INDIRECT) { p = IND_TARGET(p); p = deref(p); }
    return p;
}

obj_t *deref_pop(void)
{
    obj_t *p = pop();
    return deref(p);
}

obj_t *deref_rpop(void)
{
    obj_t *p = rpop();
    return deref(p);
}

/* --- strings ------------------------------------------------------------------------------ */
/* djb2, as HEAP.md specifies: h = 5381; h = h * 33 + c. The card never computes
 * it; LOOKUP takes the search hash as a parameter. */
uint32_t hash_str(obj_t *s)
{
    uint32_t h = 5381;
    uint16_t n = AUX16(s), i;
    const uint8_t *b = (const uint8_t *)STRBYTES(s);
    for (i = 0; i < n; ++i) h = (h << 5) + h + b[i];
    return h;
}

int str_eq(obj_t *a, obj_t *b)
{
    uint16_t n = AUX16(a);
    if (AUX16(b) != n) return 0;
    return memcmp(STRBYTES(a), STRBYTES(b), n) == 0;
}

int str_cmp(obj_t *a, obj_t *b)
{
    uint16_t na = AUX16(a), nb = AUX16(b), n = na < nb ? na : nb;
    int c = memcmp(STRBYTES(a), STRBYTES(b), n);
    if (c < 0) return 0;
    if (c > 0) return 2;
    if (na < nb) return 0;
    if (na > nb) return 2;
    return 1;
}

obj_t *str_concat(obj_t *a, obj_t *b)
{
    uint16_t na = AUX16(a), nb = AUX16(b);
    obj_t *o;
    if ((uint16_t)(na + nb) < na) nomem();
    o = str_alloc(na + nb);
    memcpy(STRBYTES(o), STRBYTES(a), na);
    memcpy(STRBYTES(o) + na, STRBYTES(b), nb);
    return o;
}

/* --- attribute sets ----------------------------------------------------------------------- */
/* Entry i of a fresh ATTRS of n entries: hash, name, value (originals). */
void attrs_put(obj_t *set, uint16_t n, uint16_t i, obj_t *name, obj_t *value)
{
    HASHES(set)[i] = hash_str(deref(name));
    ANAMES(set, n)[i] = name;
    AVALUES(set, n)[i] = value;
}

/* A linear scan of the hash column, then the name compare. */
static int scan_attr(obj_t *sv, obj_t *nv, uint32_t h, uint16_t *ix, obj_t **val)
{
    uint16_t n = AUX16(sv), i;
    for (i = 0; i < n; ++i)
        if (HASHES(sv)[i] == h && str_eq(deref(ANAMES(sv, n)[i]), nv)) {
            *ix  = i;
            *val = AVALUES(sv, n)[i];
            return 1;
        }
    return 0;
}

/* Is the STRING at card address `at` the same text as the search name, without
 * touching the set it came from? Written for the MEMCMP experiment below and
 * kept because it is the only caller of the verb; find_attr does NOT use it,
 * for the reason recorded in CARD-ISA.md: the set is already in a window by the
 * time a lookup happens, so reading fields back across the bus one at a time is
 * strictly more expensive than the copy it was meant to avoid.
 *
 *
 * The point of doing this here rather than in scan_attr is that it never touches
 * the SET: a header read for the length, then either the card's MEMCMP (when the
 * search name is itself mature, which a constant from the .nbc is after the
 * first collection) or a view of just the candidate name. Either way the cost is
 * independent of how many attributes the set has -- which is the whole reason
 * LOOKUP did not pay (docs/CARD-ISA.md). */
static int card_name_eq(uint32_t at, obj_t *name, obj_t *nv)
{
    uint32_t hdr;
    uint16_t len = AUX16(nv);
    card_read_at(at, &hdr, 4);
    if ((uint8_t)hdr != T_STRING) return 0;
    if ((uint16_t)((hdr >> 8) & 0xFFFFUL) != len) return 0;    /* lengths differ */
    if (len == 0) return 1;
    if (IS_MATURE(name))
        return card_memcmp(at + 4, PBITS(name) + 4, len) == 0;
    /* A young search name cannot be a MEMCMP operand -- the card cannot read CPU
     * RAM -- so compare against a view of the candidate name alone. */
    return str_eq(deref((obj_t *)at), nv);
}

/* `set` is the ORIGINAL pointer, `name` a view or an original. A young set is
 * searched by the CPU (HEAP.md); a mature one by the card's LOOKUP, the 32-lane
 * CAM, which returns a candidate index the name compare confirms. A hash
 * collision on a different name falls back to the scan, so a collision can
 * never lose an attribute. */
/* The set is the ORIGINAL pointer, never a view. On a hit both the index and
 * the VALUE come back, so callers need not deref the set to read it -- on the
 * card path nothing copies the set at all.
 *
 * Mature: the card's LOOKUP hands back the value pointer and a candidate index,
 * the candidate's name is confirmed without copying the set (card_name_eq), and
 * only a hash collision that misled the CAM falls through to a CPU scan of a
 * copy, so a collision can never lose an attribute. Young: a CPU hash scan. */
int find_attr(obj_t *set, obj_t *name, uint16_t *ix, obj_t **val)
{
    obj_t *nv = deref(name), *sv;
    uint32_t h = hash_str(nv), vaddr;
    if (IS_MATURE(set) && !verb_off(VERB_NO_LOOKUP)) {
        uint16_t cand;
        if (!card_lookup(PBITS(set), h, &cand, &vaddr)) return 0;
        sv = deref(set);
        if (cand < AUX16(sv) && str_eq(deref(ANAMES(sv, AUX16(sv))[cand]), nv)) {
            *ix  = cand;
            *val = (obj_t *)vaddr;      /* LOOKUP already dereferenced it */
            return 1;
        }
        return scan_attr(sv, nv, h, ix, val);
    }
    return scan_attr(deref(set), nv, h, ix, val);
}

/* For the callers that only want to know whether the attribute is there. */
int has_attr(obj_t *set, obj_t *name)
{
    uint16_t ix;
    obj_t *val;
    return find_attr(set, name, &ix, &val);
}

/* --- ordering ------------------------------------------------------------------------------ */
static int cmp64(obj_t *a, obj_t *b)
{
    int64_t x = INTVAL(a), y = INTVAL(b);
    if (x < y) return 0;
    if (x > y) return 2;
    return 1;
}

/* LISTs a and b, lexicographic by element with the same ordering recursively;
 * a proper prefix is less. Nix skips equal prefixes with `==` and orders by
 * `<` only at the first difference. Runs bytecode: a and b live on RSTACK. */
static int cmp_lists(obj_t *a, obj_t *b)
{
    uint16_t base = rsp, i = 0, na, nb;
    int r;
    rpush(a); rpush(b);
    for (;;) {
        na = AUX16(deref(rstack[base]));
        nb = AUX16(deref(rstack[base + 1]));
        if (i == na) { r = (i == nb) ? 1 : 0; break; }
        if (i == nb) { r = 2; break; }
        if (!values_equal(ELEMS(deref(rstack[base]))[i], ELEMS(deref(rstack[base + 1]))[i])) {
            push(ELEMS(deref(rstack[base]))[i]);
            push(ELEMS(deref(rstack[base + 1]))[i]);
            r = cmp_values();
            break;
        }
        ++i;
    }
    rsp = base;
    return r;
}

/* Pops and forces both; 0 (a < b), 1 (equal), 2 (a > b). INTs, STRINGs by
 * bytes, or LISTs. */
int cmp_values(void)
{
    obj_t *a, *b, *ao, *bo;
    force_two_orig(&a, &b, &ao, &bo);
    if (a->tag != b->tag) type_err();
    switch (a->tag) {
    case T_INT:    return cmp64(a, b);
    case T_STRING: return str_cmp(a, b);
    case T_LIST:   return cmp_lists(ao, bo);
    default:       type_err(); return 1;
    }
}

/* --- equality --------------------------------------------------------------------------- */
/* Nix's `==`: forces both; INT/BOOL/NULL by value, STRING by bytes, LIST and
 * ATTRS element-wise (recursively), functions never equal unless the same
 * object. */
int values_equal(obj_t *a, obj_t *b)
{
    uint16_t base = rsp, i, n;
    int r;
    obj_t *av, *bv;
    rpush(a);                       /* a is a root while b is forced */
    push(b); force_top(); b = pop();
    rpush(b);                       /* b is a root while a is forced */
    push(rstack[base]); force_top(); a = pop();
    b = rstack[base + 1];
    rsp = base;
    if (a == b) return 1;           /* the same object (originals, not views) */
    av = deref(a); bv = deref(b);
    if (av->tag != bv->tag) return 0;
    switch (av->tag) {
    case T_INT:    return INTVAL(av) == INTVAL(bv);
    case T_BOOL:   return BOOLVAL(av) == BOOLVAL(bv);
    case T_NULL:   return 1;
    case T_STRING: return str_eq(av, bv);
    case T_LIST:
        n = AUX16(av);
        if (AUX16(bv) != n) return 0;
        rpush(a); rpush(b);
        r = 1;
        for (i = 0; i < n; ++i)
            if (!values_equal(ELEMS(deref(rstack[base]))[i], ELEMS(deref(rstack[base + 1]))[i])) { r = 0; break; }
        rsp = base;
        return r;
    case T_ATTRS:
        n = AUX16(av);
        if (AUX16(bv) != n) return 0;
        rpush(a); rpush(b);
        r = 1;
        for (i = 0; i < n; ++i) {
            obj_t *sa = deref(rstack[base]), *sb = deref(rstack[base + 1]);
            if (!str_eq(deref(ANAMES(sa, n)[i]), deref(ANAMES(sb, n)[i]))) { r = 0; break; }
            if (!values_equal(AVALUES(sa, n)[i], AVALUES(sb, n)[i])) { r = 0; break; }
        }
        rsp = base;
        return r;
    default:
        return 0;
    }
}

/* --- deep force ------------------------------------------------------------------------ */
/* Force the top of the stack and, recursively, every element of a LIST and
 * every value of an ATTRS. The stack top is unchanged (its object has been
 * updated in place). A container being visited carries the mark in aux2, so a
 * cyclic structure terminates as Nix's deepSeq does. */
void deep_force_top(void)
{
    obj_t *o;
    uint16_t base = rsp, n, i;
    force_top();
    o = deref(peek());
    if (o->tag != T_LIST && o->tag != T_ATTRS) return;
    if (o->aux2 & AUX_MARK) return;             /* a cycle */
    n = AUX16(o);                               /* while the view is still fresh */
    store_byte(peek(), 3, (uint8_t)(o->aux2 | AUX_MARK));   /* on the card too, if mature */
    rpush(peek());
    for (i = 0; i < n; ++i) {
        o = deref(rstack[base]);
        push(o->tag == T_LIST ? ELEMS(o)[i] : AVALUES(o, n)[i]);
        deep_force_top();
        pop();
    }
    o = deref(rstack[base]);
    store_byte(rstack[base], 3, (uint8_t)(o->aux2 & ~AUX_MARK));
    rsp = base;
}
