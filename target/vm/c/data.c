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

/* --- pointers and stacks ---------------------------------------------------------------- */
/* Slice 1 handles young and static pointers only; a mature pointer or a null
 * is an error, never a guess. */
obj_t *deref(obj_t *p)
{
    if (p == 0 || !IS_RAM(p)) badnbc();
    return p;
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
/* Entry i of an ATTRS of n entries: hash, name, value. */
void attrs_put(obj_t *set, uint16_t n, uint16_t i, obj_t *name, obj_t *value)
{
    HASHES(set)[i] = hash_str(name);
    ANAMES(set, n)[i] = name;
    AVALUES(set, n)[i] = value;
}

/* A linear scan of the hash column, then the name compare (HEAP.md: young
 * sets are searched by the CPU; LOOKUP serves promoted ones). */
int find_attr(obj_t *set, obj_t *name, uint16_t *ix)
{
    uint32_t h = hash_str(name);
    uint16_t n = AUX16(set), i;
    uint32_t *hashes = HASHES(set);
    obj_t **names = ANAMES(set, n);
    for (i = 0; i < n; ++i)
        if (hashes[i] == h && str_eq(deref(names[i]), name)) { *ix = i; return 1; }
    return 0;
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
    obj_t *a, *b;
    force_two(&a, &b);
    if (a->tag != b->tag) type_err();
    switch (a->tag) {
    case T_INT:    return cmp64(a, b);
    case T_STRING: return str_cmp(a, b);
    case T_LIST:   return cmp_lists(a, b);
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
    rpush(a);                       /* a is a root while b is forced */
    push(b); force_top(); b = pop();
    rpush(b);                       /* b is a root while a is forced */
    push(rstack[base]); force_top(); a = pop();
    b = rstack[base + 1];
    rsp = base;
    a = deref(a); b = deref(b);
    if (a == b) return 1;
    if (a->tag != b->tag) return 0;
    switch (a->tag) {
    case T_INT:    return INTVAL(a) == INTVAL(b);
    case T_BOOL:   return BOOLVAL(a) == BOOLVAL(b);
    case T_NULL:   return 1;
    case T_STRING: return str_eq(a, b);
    case T_LIST:
        n = AUX16(a);
        if (AUX16(b) != n) return 0;
        rpush(a); rpush(b);
        r = 1;
        for (i = 0; i < n; ++i)
            if (!values_equal(ELEMS(deref(rstack[base]))[i], ELEMS(deref(rstack[base + 1]))[i])) { r = 0; break; }
        rsp = base;
        return r;
    case T_ATTRS:
        n = AUX16(a);
        if (AUX16(b) != n) return 0;
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
    o->aux2 |= AUX_MARK;
    n = AUX16(o);
    rpush(peek());
    for (i = 0; i < n; ++i) {
        o = deref(rstack[base]);
        push(o->tag == T_LIST ? ELEMS(o)[i] : AVALUES(o, n)[i]);
        deep_force_top();
        pop();
    }
    o = deref(rstack[base]);
    o->aux2 &= (uint8_t)~AUX_MARK;
    rsp = base;
}
