/* prims.c -- the primops (BYTECODE.md "Primops"). Translated from the
 * assembly VM's prims.s (git: commit 05ce616).
 *
 * PRIM id: the arguments are on the value stack, first argument deepest,
 * unforced. Each primop pops what it takes, forces what it needs and pushes one
 * value. Loops that call back into Nix (map, filter, foldl', any, all, ...)
 * keep their objects on the root stack, because do_call may collect -- and may
 * PROMOTE: a container being filled across a callback could become mature
 * mid-loop, so those loops collect their results on the value stack and build
 * the container afterwards, when nothing runs bytecode any more.
 *
 * Originals and views (vm.h deref): a value popped from the stack is stored
 * (rpush, push, a field); it is read through deref(). force_pop_view gives
 * both.
 */
#pragma noroot
#pragma optimize 15

#include "prims.h"

/* The primops are a load segment of their own: omf2bin puts it in a bank above
 * bank 0, whose 40 KB hold the interpreter core and every global (VM.md). */
segment "prims";

/* --- static strings the VM hands out ------------------------------------------------- */
/* Header (aux = length), bytes zero-padded, a null context in the zero bytes
 * that follow. Immutable. */
#define SSTR(name, text) \
    static struct { obj_t h; char s[16]; } name##_s = { { T_STRING, sizeof(text) - 1, 0, 0 }, text }

SSTR(str_int, "int");
SSTR(str_bool, "bool");
SSTR(str_null, "null");
SSTR(str_string, "string");
SSTR(str_list, "list");
SSTR(str_set, "set");
SSTR(str_lambda, "lambda");
SSTR(str_path, "path");
SSTR(str_tostring, "__toString");
SSTR(str_outpath, "outPath");
SSTR(str_one, "1");
SSTR(str_dot, ".");
SSTR(str_slash, "/");
SSTR(str_name, "name");
SSTR(str_right, "right");
SSTR(str_wrong, "wrong");
SSTR(str_space, " ");
SSTR(str_type, "type");
SSTR(str_float, "float");

obj_t *str_int, *str_bool, *str_null, *str_string, *str_list, *str_set, *str_lambda,
             *str_one, *str_dot, *str_slash, *str_name, *str_right, *str_wrong, *str_space, *str_path,
             *str_tostring, *str_outpath, *str_type, *str_float;

void prims_init(void)
{
    str_int    = STATIC_PTR(&str_int_s);
    str_bool   = STATIC_PTR(&str_bool_s);
    str_null   = STATIC_PTR(&str_null_s);
    str_string = STATIC_PTR(&str_string_s);
    str_list   = STATIC_PTR(&str_list_s);
    str_set    = STATIC_PTR(&str_set_s);
    str_lambda = STATIC_PTR(&str_lambda_s);
    str_path   = STATIC_PTR(&str_path_s);
    str_tostring = STATIC_PTR(&str_tostring_s);
    str_outpath  = STATIC_PTR(&str_outpath_s);
    str_one    = STATIC_PTR(&str_one_s);
    str_dot    = STATIC_PTR(&str_dot_s);
    str_slash  = STATIC_PTR(&str_slash_s);
    str_name   = STATIC_PTR(&str_name_s);
    str_right  = STATIC_PTR(&str_right_s);
    str_wrong  = STATIC_PTR(&str_wrong_s);
    str_space  = STATIC_PTR(&str_space_s);
    str_type   = STATIC_PTR(&str_type_s);
    str_float  = STATIC_PTR(&str_float_s);
    drv_table  = 0;
}

/* --- shared ---------------------------------------------------------------------------- */
static void push_bool(int v) { push(v ? obj_true : obj_false); }


/* Force and pop; the original in *orig, the view returned, type-checked. */
obj_t *force_pop_as(uint8_t tag, obj_t **orig)
{
    obj_t *v;
    *orig = force_pop_view(&v);
    need(v, tag);
    return v;
}

/* An INT that is an index or a length: 0..65535, else an index error. */
static uint16_t pop_index(void)
{
    obj_t *o, *v = force_pop_as(T_INT, &o);
    int64_t x = INTVAL(v);
    if (x < 0 || x > 0xFFFF) err_exit(E_INDEX);
    return (uint16_t)x;
}

static void binary_ints(obj_t **a, obj_t **b)
{
    force_two(a, b);
    need(*a, T_INT);
    need(*b, T_INT);
}

/* f x: pushes both, calls; the value is on top. Runs bytecode. */
static void call_with(obj_t *f, obj_t *x)
{
    push(f);
    push(x);
    do_call();
}


/* The n values on top of the stack become a fresh LIST. */
obj_t *list_from_stack(uint16_t n)
{
    obj_t *o;
    nursery_reserve_n(n, 4, 4);                 /* the n values are on the stack: roots */
    o = list_alloc(n);
    vsp -= n;
    memcpy(ELEMS(o), &vstack[vsp], 4 * n);
    return o;
}

/* Entry i of an ATTRS of n entries copied from src (ns entries, index j). */
static void copy_entry(obj_t *dst, uint16_t n, uint16_t i, obj_t *src, uint16_t ns, uint16_t j)
{
    HASHES(dst)[i] = HASHES(src)[j];
    ANAMES(dst, n)[i] = ANAMES(src, ns)[j];
    AVALUES(dst, n)[i] = AVALUES(src, ns)[j];
}

/* --- lists --------------------------------------------------------------------------- */
static void prim_length(void)
{
    obj_t *o, *l = force_pop_as(T_LIST, &o);
    push(new_int(AUX16(l)));
}

static void prim_head(void)
{
    obj_t *o, *l = force_pop_as(T_LIST, &o);
    if (AUX16(l) == 0) err_exit(E_INDEX);
    push(ELEMS(l)[0]);
}

/* Force the top in place, type-check it, and view it -- WITHOUT popping, so it
 * stays a root while the primop reserves room for what it will allocate. After
 * a reserve that collected, view it again. (force_pop_as, but for primops that
 * allocate in proportion to their argument: vm.h nursery_reserve.) */
static obj_t *view_top_as(uint8_t tag)
{
    obj_t *v;
    force_top();
    v = deref(peek());
    need(v, tag);
    return v;
}

/* view_top_as for primops that take a STRING or a PATH (dirOf, baseNameOf). */
obj_t *view_top_text(void)
{
    obj_t *v;
    force_top();
    v = deref(peek());
    if (v->tag != T_STRING && v->tag != T_PATH) type_err();
    return v;
}

static void prim_tail(void)
{
    obj_t *l = view_top_as(T_LIST), *r;
    uint16_t n = AUX16(l);
    if (n == 0) err_exit(E_INDEX);
    if (nursery_reserve_n(n - 1, 4, 4)) l = view_top_as(T_LIST);
    --vsp;
    r = list_alloc(n - 1);
    memcpy(ELEMS(r), ELEMS(l) + 1, 4 * (n - 1));
    push(r);
}

static void prim_elemat(void)                   /* list i */
{
    uint16_t i = pop_index();
    obj_t *o, *l = force_pop_as(T_LIST, &o);
    if (i >= AUX16(l)) err_exit(E_INDEX);
    push(ELEMS(l)[i]);
}

/* make_apply allocates 28 bytes (an environment of 3 and a thunk): map is
 * 32 bytes an element, genList 44 with its INT -- 13 KB for genList 300, which
 * is why these two reserve before they start (vm.h nursery_reserve). */
static void prim_map(void)                      /* f list -> a list of (f x) thunks */
{
    obj_t *l = view_top_as(T_LIST), *f, *r;
    uint16_t n = AUX16(l), i;
    if (nursery_reserve_n(n, 32, 4)) l = view_top_as(T_LIST);
    --vsp;
    f = pop();
    r = list_alloc(n);
    for (i = 0; i < n; ++i) ELEMS(r)[i] = make_apply(f, ELEMS(l)[i]);
    push(r);
}

static void prim_genlist(void)                  /* f n -> [ (f 0) ... (f (n-1)) ] */
{
    uint16_t n = pop_index(), i;
    obj_t *f, *r;
    nursery_reserve_n(n, 44, 4);                /* f is still on the stack: a root */
    f = pop();
    r = list_alloc(n);
    for (i = 0; i < n; ++i) ELEMS(r)[i] = make_apply(f, new_int(i));
    push(r);
}

static void prim_filter(void)                   /* f list. Roots: list, f; kept elements on the stack */
{
    uint16_t b = rsp, n, i, kept = 0;
    obj_t *lo, *l = force_pop_as(T_LIST, &lo), *v, *vo;
    n = AUX16(l);
    rpush(lo);
    rpush(pop());                               /* f */
    for (i = 0; i < n; ++i) {
        call_with(rstack[b + 1], RELEM(b, i, n));
        v = force_pop_as(T_BOOL, &vo);
        if (BOOLVAL(v)) { push(RELEM(b, i, n)); ++kept; }
    }
    rsp = b;
    push(list_from_stack(kept));
}

static void prim_foldl(void)                    /* f z list: the accumulator lives on the value stack */
{
    uint16_t b = rsp, n, i;
    obj_t *lo, *l = force_pop_as(T_LIST, &lo), *z;
    n = AUX16(l);
    rpush(lo);
    z = pop();
    rpush(pop());                               /* f */
    push(z);
    for (i = 0; i < n; ++i) {
        obj_t *acc = pop();
        call_with(rstack[b + 1], acc);          /* g = f acc */
        push(RELEM(b, i, n));
        do_call();                              /* acc' = g x */
        force_top();                            /* strict accumulator */
    }
    rsp = b;
}

static void prim_concatlists(void)              /* [ [..] [..] ]: force each, sum, copy */
{
    uint16_t b = rsp, n, i, total = 0, w = 0;
    obj_t *lo, *l = force_pop_as(T_LIST, &lo), *e, *eo, *r;
    n = AUX16(l);
    rpush(lo);
    for (i = 0; i < n; ++i) {
        push(RELEM(b, i, n));
        e = force_pop_as(T_LIST, &eo);
        if ((uint16_t)(total + AUX16(e)) < total) nomem();
        total += AUX16(e);
    }
    nursery_reserve_n(total, 4, 4);             /* all rooted on RSTACK; the copy re-fetches */
    r = list_alloc(total);
    for (i = 0; i < n; ++i) {
        e = deref_follow(RELEM(b, i, n));
        memcpy(ELEMS(r) + w, ELEMS(e), 4 * AUX16(e));
        w += AUX16(e);
    }
    rsp = b;
    push(r);
}

static void prim_concatmap(void)                /* concatLists (map f list), the map strict */
{
    uint16_t b = rsp, n, i;
    obj_t *lo, *l = force_pop_as(T_LIST, &lo);
    n = AUX16(l);
    rpush(lo);
    rpush(pop());                               /* f */
    for (i = 0; i < n; ++i) {
        call_with(rstack[b + 1], RELEM(b, i, n));
        force_top();                            /* the result stays on the stack */
    }
    rsp = b;
    push(list_from_stack(n));
    prim_concatlists();
}

static void prim_elem(void)                     /* x list */
{
    uint16_t b = rsp, n, i;
    obj_t *lo, *l = force_pop_as(T_LIST, &lo);
    int found = 0;
    n = AUX16(l);
    rpush(lo);
    rpush(pop());                               /* x */
    for (i = 0; i < n; ++i)
        if (values_equal(RELEM(b, i, n), rstack[b + 1])) { found = 1; break; }
    rsp = b;
    push_bool(found);
}

static void any_all(int want)                   /* f list; `want` decides early */
{
    uint16_t b = rsp, n, i;
    obj_t *lo, *l = force_pop_as(T_LIST, &lo), *v, *vo;
    int r = !want;
    n = AUX16(l);
    rpush(lo);
    rpush(pop());                               /* f */
    for (i = 0; i < n; ++i) {
        call_with(rstack[b + 1], RELEM(b, i, n));
        v = force_pop_as(T_BOOL, &vo);
        if ((BOOLVAL(v) != 0) == want) { r = want; break; }
    }
    rsp = b;
    push_bool(r);
}

/* --- attribute sets ---------------------------------------------------------------------- */
static void prim_attrnames(void)
{
    obj_t *s = view_top_as(T_ATTRS), *r;
    uint16_t n = AUX16(s);
    if (nursery_reserve_n(n, 4, 4)) s = view_top_as(T_ATTRS);
    --vsp;
    r = list_alloc(n);
    memcpy(ELEMS(r), ANAMES(s, n), 4 * n);
    push(r);
}

static void prim_attrvalues(void)
{
    obj_t *s = view_top_as(T_ATTRS), *r;
    uint16_t n = AUX16(s);
    if (nursery_reserve_n(n, 4, 4)) s = view_top_as(T_ATTRS);
    --vsp;
    r = list_alloc(n);
    memcpy(ELEMS(r), AVALUES(s, n), 4 * n);
    push(r);
}

/* hasAttr name set / getAttr name set. Nix forces the NAME first, so a throwing
 * name is caught before a non-set second argument is a type error. Both
 * originals. */
static void force_name_then_set(obj_t **name, obj_t **set)
{
    obj_t *v;
    force_slot(2);
    *set = force_pop_view(&v);
    need(v, T_ATTRS);
    *name = pop();
    v = deref(*name);
    need(v, T_STRING);
    if (CTX(v)) type_err();                     /* forceStringNoCtx: a name has no context */
}

static void prim_hasattr(void)
{
    obj_t *name, *set;
    force_name_then_set(&name, &set);
    push_bool(has_attr(set, name));
}

static void prim_getattr(void)
{
    obj_t *name, *set, *v;
    uint16_t ix;
    force_name_then_set(&name, &set);
    if (!find_attr(set, name, &ix, &v)) err_exit(E_NOATTR);
    push(v);
}

static void prim_intersectattrs(void)           /* names of a that are in b, with b's values */
{
    obj_t *a, *b, *ao, *bo, *r, *v;
    uint16_t na, nb, i, j, k = 0, rb = rsp;
    force_two_orig(&a, &b, &ao, &bo);
    need(a, T_ATTRS); need(b, T_ATTRS);
    na = AUX16(a); nb = AUX16(b);
    for (i = 0; i < na; ++i) {
        a = deref(ao);
        if (has_attr(bo, ANAMES(a, na)[i])) ++k;
    }
    rpush(ao); rpush(bo);                       /* rooted across the reserve */
    nursery_reserve_n(k, 12, 4);
    ao = rstack[rb]; bo = rstack[rb + 1];
    rsp = rb;
    r = attrs_alloc(k);
    for (i = 0, k = 0; i < na; ++i) {
        a = deref(ao);
        if (find_attr(bo, ANAMES(a, na)[i], &j, &v)) { b = deref(bo); copy_entry(r, AUX16(r), k, b, nb, j); ++k; }
    }
    push(r);
}

/* Is the STRING `name` (a view) among the (forced) elements of LIST l (original)? */
static int name_in_list(obj_t *name, obj_t *lo)
{
    uint16_t n = AUX16(deref(lo)), j;
    /* list_elem, not deref(lo) each time round: a long mature list evicted by
     * the element derefs is re-copied whole per element (data.c list_elem). */
    for (j = 0; j < n; ++j)
        if (str_eq(name, deref_follow(list_elem(lo, j, n)))) return 1;
    return 0;
}

static void prim_removeattrs(void)              /* set [ names ]: keep entries not in the list */
{
    uint16_t b = rsp, ns, nl, i, k = 0;
    obj_t *s, *l, *so, *lo, *r, *e, *eo;
    force_two_orig(&s, &l, &so, &lo);
    need(s, T_ATTRS); need(l, T_LIST);
    nl = AUX16(l);
    rpush(so);
    rpush(lo);
    for (i = 0; i < nl; ++i) {                  /* force every name */
        push(RELEM(b + 1, i, nl));
        e = force_pop_as(T_STRING, &eo);
        if (CTX(e)) type_err();                 /* forceStringNoCtx */
    }
    so = rstack[b];
    lo = rstack[b + 1];
    s = deref(so);
    ns = AUX16(s);
    for (i = 0; i < ns; ++i) {
        s = deref(so);
        if (!name_in_list(deref(ANAMES(s, ns)[i]), lo)) ++k;
    }
    nursery_reserve_n(k, 12, 4);                /* so and lo are still on RSTACK */
    so = rstack[b];
    lo = rstack[b + 1];
    rsp = b;
    r = attrs_alloc(k);
    for (i = 0, k = 0; i < ns; ++i) {
        s = deref(so);
        if (!name_in_list(deref(ANAMES(s, ns)[i]), lo)) { s = deref(so); copy_entry(r, AUX16(r), k, s, ns, i); ++k; }
    }
    push(r);
}

/* Sorted insertion into the fresh ATTRS `set` of capacity `cap` holding *cnt
 * entries; a duplicate name is dropped. Nothing here runs bytecode. */
void insert_entry(obj_t *set, uint16_t cap, uint16_t *cnt, obj_t *name, obj_t *value)
{
    uint16_t j, k, c;
    obj_t *nv = deref(name);
    for (j = 0; j < *cnt; ++j) {
        c = str_cmp(deref(ANAMES(set, cap)[j]), nv);
        if (c == 1) return;                     /* first occurrence wins */
        if (c == 2) break;                      /* existing > new: insert here */
    }
    for (k = *cnt; k > j; --k) {
        HASHES(set)[k] = HASHES(set)[k - 1];
        ANAMES(set, cap)[k] = ANAMES(set, cap)[k - 1];
        AVALUES(set, cap)[k] = AVALUES(set, cap)[k - 1];
    }
    HASHES(set)[j] = hash_str(nv);
    ANAMES(set, cap)[j] = name;
    AVALUES(set, cap)[j] = value;
    ++*cnt;
}

/* --- computed attribute names (BYTECODE.md "Dynamic attribute names") ------------------ */
/* ATTR_NAME: a computed key in a selection, forced. SELECT, TRY_SELECT and
 * HAS_ATTR take nothing but a STRING; this adds the rest of Nix's
 * forceStringNoCtx, no context. The key stays on the stack for them. */
void attr_name(void)
{
    obj_t *v;
    force_top();
    v = deref(peek());
    if (v->tag == T_STRING && CTX(v)) type_err();
}

/* ATTRS_DYN n m: n (name, value) pairs with static names, sorted, then m whose
 * names the generator computed and forced (a THUNK here is its bug: E_TYPE). A null name drops its pair; any
 * other must be a STRING without context; a name already in the set -- static
 * or computed -- is E_DUPATTR, Nix's "dynamic attribute already defined".
 * Everything is on the stack, a root, while the set is reserved. The operands
 * are read here, not in run(): each fetch there is bank-0 code. */
void attrs_dynamic(void)
{
    uint16_t n = pc[0] | ((uint16_t)pc[1] << 8), m = pc[2] | ((uint16_t)pc[3] << 8);
    uint16_t at = vsp - 2 * (n + m), keep = n, cnt = 0, before, i;
    obj_t *nm, *set;
    pc += 4;
    for (i = n; i < n + m; ++i) {
        nm = deref(vstack[at + 2 * i]);
        if (nm->tag == T_NULL) continue;
        if (nm->tag != T_STRING || CTX(nm)) type_err();
        ++keep;
    }
    nursery_reserve_n(keep, 12, 4);             /* as ATTRS */
    set = attrs_alloc(keep);
    for (i = 0; i < n + m; ++i) {
        nm = vstack[at + 2 * i];
        if (i >= n && deref(nm)->tag == T_NULL) continue;
        before = cnt;
        insert_entry(set, keep, &cnt, nm, vstack[at + 2 * i + 1]);
        if (cnt == before) err_exit(E_DUPATTR);
    }
    vsp = at;
    push(set);
}

static void prim_listtoattrs(void)              /* [ { name; value; } ... ] */
{
    uint16_t b = rsp, n, i, cnt = 0, ix, at;
    obj_t *lo, *l = force_pop_as(T_LIST, &lo), *eo, *nm, *no, *set, *av;
    n = AUX16(l);
    rpush(lo);
    /* pass 1, running bytecode: (name, value) pairs onto the value stack */
    for (i = 0; i < n; ++i) {
        push(RELEM(b, i, n));
        (void)force_pop_as(T_ATTRS, &eo);       /* forced and checked */
        rpush(eo);                              /* b+1: the element */
        if (!find_attr(eo, str_name, &ix, &av)) err_exit(E_NOATTR);
        push(av);
        nm = force_pop_as(T_STRING, &no);
        if (CTX(nm)) type_err();                /* forceStringNoCtx */
        push(no);                               /* the name */
        if (!find_attr(rstack[b + 1], str_value, &ix, &av)) err_exit(E_NOATTR);
        push(av);                               /* the value, unforced */
        rsp = b + 1;
    }
    rsp = b;
    /* pass 2, no bytecode: sorted insertion into a fresh set */
    at = vsp - 2 * n;
    nursery_reserve_n(n, 24, 8);                /* the set and its compacted copy; pairs are on the stack */
    set = attrs_alloc(n);                       /* capacity n; column offsets use n */
    for (i = 0; i < n; ++i)
        insert_entry(set, n, &cnt, vstack[at + 2 * i], vstack[at + 2 * i + 1]);
    vsp = at;
    if (cnt != n) {                             /* duplicates were dropped: compact */
        obj_t *r = attrs_alloc(cnt);
        for (i = 0; i < cnt; ++i) copy_entry(r, cnt, i, set, n, i);
        set = r;
    }
    push(set);
}

static void prim_partition(void)                /* f list -> { right; wrong; } */
{
    uint16_t b = rsp, n, i, nr = 0, nw = 0, at;
    obj_t *lo, *l = force_pop_as(T_LIST, &lo), *v, *vo, *r, *w;
    n = AUX16(l);
    rpush(lo);
    rpush(pop());                               /* f */
    for (i = 0; i < n; ++i) {                   /* (element, verdict) pairs on the stack */
        call_with(rstack[b + 1], RELEM(b, i, n));
        v = force_pop_as(T_BOOL, &vo);
        push(RELEM(b, i, n));
        push(BOOLVAL(v) ? obj_true : obj_false);
    }
    rsp = b;
    at = vsp - 2 * n;
    for (i = 0; i < n; ++i) if (vstack[at + 2 * i + 1] == obj_true) ++nr; else ++nw;
    nursery_reserve_n(n, 4, 8 + 28);            /* both lists and the 2-entry set; pairs on the stack */
    r = list_alloc(nr);
    w = list_alloc(nw);
    for (i = 0, nr = 0, nw = 0; i < n; ++i) {
        if (vstack[at + 2 * i + 1] == obj_true) ELEMS(r)[nr++] = vstack[at + 2 * i];
        else                                     ELEMS(w)[nw++] = vstack[at + 2 * i];
    }
    vsp = at;
    v = attrs_alloc(2);
    attrs_put(v, 2, 0, str_right, r);
    attrs_put(v, 2, 1, str_wrong, w);
    push(v);
}

/* --- errors, forcing, types --------------------------------------------------------------- */
_Noreturn static void throw_exit(uint8_t code)
{
    obj_t *so, *s = force_pop_as(T_STRING, &so);
    *(obj_t **)RESULT_ADDR = so;                /* the message travels in RESULT */
    (void)s;
    err_exit(code);
}

static void prim_seq(void)                      /* a b: force a, return b */
{
    rpush(pop());
    force_top();
    pop();
    push(rpop());
}

static void prim_deepseq(void)
{
    rpush(pop());
    deep_force_top();
    pop();
    push(rpop());
}

static void prim_typeof(void)
{
    obj_t *v;
    (void)force_pop_view(&v);
    switch (v->tag) {
    case T_INT:    push(str_int); break;
    case T_FLOAT:  push(str_float); break;
    case T_BOOL:   push(str_bool); break;
    case T_NULL:   push(str_null); break;
    case T_STRING: push(str_string); break;
    case T_LIST:   push(str_list); break;
    case T_ATTRS:  push(str_set); break;
    case T_LAMBDA: push(str_lambda); break;
    case T_PATH:   push(str_path); break;
    default:       type_err();
    }
}

static void is_tag(uint8_t tag)
{
    obj_t *v;
    (void)force_pop_view(&v);
    push_bool(v->tag == tag);
}

/* tryEval: save a frame, force the argument; a throw or a failed assert inside
 * unwinds here (err_exit) with everything restored, and the result set says
 * so. { success = ...; value = ...; } */
static void prim_tryeval(void)
{
    struct catch_frame *c;
    obj_t *v, *o;
    int ok;
    if (catch_n >= CATCH_N) nomem();
    c = &catches[catch_n];
    c->vsp = vsp;                               /* the argument is still on the stack */
    c->env = env;
    c->pc = pc;
    c->ctab = ctab;
    c->fstack_n = fstack_n;
    c->rsp = rsp;
    ++catch_n;
    if (setjmp(c->jb) == 0) {
        force_top();
        --catch_n;
        v = pop();
        ok = 1;
    } else {
        pop();                                  /* the argument's slot */
        v = obj_false;
        ok = 0;
    }
    rpush(v);
    o = attrs_alloc(2);
    attrs_put(o, 2, 0, str_success, ok ? obj_true : obj_false);
    attrs_put(o, 2, 1, str_value, rpop());
    push(o);
}

/* --- arithmetic --------------------------------------------------------------------------- */
/* Nix 2.34 checks + - * / for overflow and makes it an error, never a wrap
 * (primops.cc, eval.cc: "integer overflow in adding ..."). run.c does the
 * arithmetic and calls these, which are here because bank 0 is full. For +
 * and - the sign bits decide: a sum overflows when both operands have one
 * sign and the result the other, a difference when the operands' signs differ
 * and the result's is not the first's. */
#define SIGN(x) (BYTE_OF(x, 7) & 0x80)
int64_t add_checked(int64_t x, int64_t y)
{
    int64_t r = (int64_t)((uint64_t)x + (uint64_t)y);
    if (SIGN(x) == SIGN(y) && SIGN(r) != SIGN(x)) err_exit(E_OVERFLOW);
    return r;
}

int64_t sub_checked(int64_t x, int64_t y)
{
    int64_t r = (int64_t)((uint64_t)x - (uint64_t)y);
    if (SIGN(x) != SIGN(y) && SIGN(r) != SIGN(x)) err_exit(E_OVERFLOW);
    return r;
}

/* Did a * b or a / b leave the 64-bit range? (Nix 2.34: "integer overflow in
 * multiplying", "... dividing".) a and b are 8-byte little-endian INT
 * payloads; run.c has the low 64 bits of the answer already, from the card's
 * ALU64 or alu_cpu, and calls this after every MUL and DIV. */
static int fits32(const uint8_t *x)             /* -2^31 .. 2^31 - 1 */
{
    return x[3] & 0x80 ? (x[4] & x[5] & x[6] & x[7]) == 0xFF : (x[4] | x[5] | x[6] | x[7]) == 0;
}

static uint64_t magnitude(const uint8_t *x)     /* |x|; the minimum's is 2^63 */
{
    int64_t v;
    memcpy(&v, x, 8);
    return x[7] & 0x80 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
}

void alu_overflow(uint8_t op, const uint8_t *a, const uint8_t *b)
{
    uint64_t ua, ub, lim;
    if (op != A_MUL) {                          /* only the minimum / -1 does not fit */
        if ((b[0] & b[1] & b[2] & b[3] & b[4] & b[5] & b[6] & b[7]) == 0xFF &&
            a[7] == 0x80 && (a[0] | a[1] | a[2] | a[3] | a[4] | a[5] | a[6]) == 0)
            err_exit(E_OVERFLOW);
        return;
    }
    if (fits32(a) && fits32(b)) return;         /* |ab| <= 2^62: nearly every multiply */
    ua = magnitude(a);
    ub = magnitude(b);
    if (ua == 0 || ub == 0) return;
    /* |ab| may reach 2^63 when the product is negative, 2^63 - 1 otherwise. */
    lim = (a[7] ^ b[7]) & 0x80 ? (uint64_t)1 << 63 : ((uint64_t)1 << 63) - 1;
    if (ub > lim / ua) err_exit(E_OVERFLOW);
}

/* --- bit operations ---------------------------------------------------------------------- */
static void bitop(uint8_t which)
{
    obj_t *a, *b;
    int64_t r;
    binary_ints(&a, &b);
    switch (which) {
    case 0:  r = INTVAL(a) & INTVAL(b); break;
    case 1:  r = INTVAL(a) | INTVAL(b); break;
    default: r = INTVAL(a) ^ INTVAL(b); break;
    }
    push(new_int(r));
}

/* --- the table ---------------------------------------------------------------------------- */
void op_prim(void)
{
    uint16_t id = *pc++;
    switch (id) {
    case PRIM_LENGTH:          prim_length(); break;
    case PRIM_HEAD:            prim_head(); break;
    case PRIM_TAIL:            prim_tail(); break;
    case PRIM_ELEMAT:          prim_elemat(); break;
    case PRIM_MAP:             prim_map(); break;
    case PRIM_FILTER:          prim_filter(); break;
    case PRIM_FOLDL:           prim_foldl(); break;
    case PRIM_CONCATLISTS:     prim_concatlists(); break;
    case PRIM_GENLIST:         prim_genlist(); break;
    case PRIM_ELEM:            prim_elem(); break;
    case PRIM_ANY:             any_all(1); break;
    case PRIM_ALL:             any_all(0); break;
    case PRIM_CONCATMAP:       prim_concatmap(); break;
    case PRIM_ATTRNAMES:       prim_attrnames(); break;
    case PRIM_ATTRVALUES:      prim_attrvalues(); break;
    case PRIM_HASATTR:         prim_hasattr(); break;
    case PRIM_GETATTR:         prim_getattr(); break;
    case PRIM_STRINGLENGTH:    prim_stringlength(); break;
    case PRIM_SUBSTRING:       prim_substring(); break;
    case PRIM_TOSTRING:        prim_tostring(); break;
    case PRIM_CONCATSTRINGSSEP: prim_concatstringssep(); break;
    case PRIM_THROW:           throw_exit(E_THROW); break;
    case PRIM_ABORT:           throw_exit(E_ABORT); break;
    case PRIM_SEQ:             prim_seq(); break;
    case PRIM_DEEPSEQ:         prim_deepseq(); break;
    case PRIM_TYPEOF:          prim_typeof(); break;
    case PRIM_ISINT:           is_tag(T_INT); break;
    case PRIM_ISBOOL:          is_tag(T_BOOL); break;
    case PRIM_ISSTRING:        is_tag(T_STRING); break;
    case PRIM_ISNULL:          is_tag(T_NULL); break;
    case PRIM_ISLIST:          is_tag(T_LIST); break;
    case PRIM_ISATTRS:         is_tag(T_ATTRS); break;
    case PRIM_ISFUNCTION:      is_tag(T_LAMBDA); break;
    case PRIM_ADD:             op_arith(OP_ADD); break;
    case PRIM_SUB:             op_arith(OP_SUB); break;
    case PRIM_MUL:             op_arith(OP_MUL); break;
    case PRIM_DIV:             op_arith(OP_DIV); break;
    case PRIM_LESSTHAN:        op_arith(OP_LT); break;
    case PRIM_BITAND:          bitop(0); break;
    case PRIM_BITOR:           bitop(1); break;
    case PRIM_BITXOR:          bitop(2); break;
    case PRIM_LISTTOATTRS:     prim_listtoattrs(); break;
    case PRIM_INTERSECTATTRS:  prim_intersectattrs(); break;
    case PRIM_REMOVEATTRS:     prim_removeattrs(); break;
    case PRIM_PARTITION:       prim_partition(); break;
    case PRIM_BASENAMEOF:      prim_basenameof(); break;
    case PRIM_DIROF:           prim_dirof(); break;
    case PRIM_TRYEVAL:         prim_tryeval(); break;
    case PRIM_HASHSTRING:      prim_hashstring(); break;
    case PRIM_DERIVATIONSTRICT: prim_derivationstrict(); break;
    case PRIM_HASCONTEXT:      prim_hascontext(); break;
    case PRIM_UNSAFEDISCARDSTRINGCONTEXT: prim_unsafediscardstringcontext(); break;
    case PRIM_UNSAFEDISCARDOUTPUTDEPENDENCY: prim_unsafediscardoutputdependency(); break;
    case PRIM_ADDDRVOUTPUTDEPENDENCIES: prim_adddrvoutputdependencies(); break;
    case PRIM_GETCONTEXT:      prim_getcontext(); break;
    case PRIM_APPENDCONTEXT:   prim_appendcontext(); break;
    case PRIM_TOFILE:          prim_tofile(); break;
    case PRIM_ISFLOAT:         is_tag(T_FLOAT); break;
    case PRIM_FLOOR:           prim_floor(); break;
    case PRIM_CEIL:            prim_ceil(); break;
    default:                   err_exit(E_BADOP);
    }
}
