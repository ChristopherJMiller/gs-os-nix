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

#include "vm.h"

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

static obj_t *str_int, *str_bool, *str_null, *str_string, *str_list, *str_set, *str_lambda,
             *str_one, *str_dot, *str_slash, *str_name, *str_right, *str_wrong, *str_space, *str_path,
             *str_tostring, *str_outpath, *str_type;

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
    drv_table  = 0;
}

/* --- shared ---------------------------------------------------------------------------- */
static void push_bool(int v) { push(v ? obj_true : obj_false); }

/* A STRING's context (DERIVATION.md "String context"): 0, or a LIST of STRINGs
 * sorted by bytes, no two equal, in Nix's element syntax -- "!<output>!<drv>",
 * "=<drv>", "/nix/store/...". s is a STRING view. */
#define CTX(s) STRCTX((s), AUX16(s))

/* Force and pop; the original in *orig, the view returned, type-checked. */
static obj_t *force_pop_as(uint8_t tag, obj_t **orig)
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

/* The i-th element of the LIST (of n elements) whose pointer is root stack
 * entry r. Re-fetched every time round, because the loop calls back into Nix
 * (which may collect or turn the window ring over); list_elem makes that cheap
 * for a long mature list by reading one element rather than copying it. */
#define RELEM(r, i, n) list_elem(rstack[r], (i), (n))

/* The n values on top of the stack become a fresh LIST. */
static obj_t *list_from_stack(uint16_t n)
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
static obj_t *view_top_text(void)
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
    need(deref(*name), T_STRING);
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
static void insert_entry(obj_t *set, uint16_t cap, uint16_t *cnt, obj_t *name, obj_t *value)
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

static void prim_listtoattrs(void)              /* [ { name; value; } ... ] */
{
    uint16_t b = rsp, n, i, cnt = 0, ix, at;
    obj_t *lo, *l = force_pop_as(T_LIST, &lo), *e, *eo, *nm, *no, *set, *av;
    n = AUX16(l);
    rpush(lo);
    /* pass 1, running bytecode: (name, value) pairs onto the value stack */
    for (i = 0; i < n; ++i) {
        push(RELEM(b, i, n));
        e = force_pop_as(T_ATTRS, &eo);
        rpush(eo);                              /* b+1: the element */
        if (!find_attr(eo, str_name, &ix, &av)) err_exit(E_NOATTR);
        push(av);
        nm = force_pop_as(T_STRING, &no);
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

/* --- strings ---------------------------------------------------------------------------- */
static void prim_stringlength(void)
{
    obj_t *o, *s;
    coerce_top(CO_PLAIN);                       /* coerced, as Nix does: a set via __toString / outPath */
    s = force_pop_as(T_STRING, &o);
    push(new_int(AUX16(s)));
}

static void prim_substring(void)                /* start len s */
{
    uint16_t start, len = 0, slen, end;
    int to_end = 0;
    obj_t *so, *s, *vo, *v, *r;
    coerce_top(CO_PLAIN);                       /* the string argument is coerced (a set via __toString) */
    s = force_pop_as(T_STRING, &so);
    int64_t x;
    rpush(so);
    v = force_pop_as(T_INT, &vo);               /* len: negative means "to the end" */
    x = INTVAL(v);
    if (x < 0 || x > 0xFFFF) to_end = 1; else len = (uint16_t)x;
    v = force_pop_as(T_INT, &vo);               /* start */
    x = INTVAL(v);
    if (x < 0) err_exit(E_INDEX);
    s = deref(rstack[rsp - 1]);                 /* still rooted, for the reserve */
    slen = AUX16(s);
    if (x > 0xFFFF || (uint16_t)x >= slen) {   /* "", with the string's context, as Nix keeps it */
        r = str_alloc(0);
        STRCTX(r, 0) = CTX(s);
        --rsp;
        push(r);
        return;
    }
    start = (uint16_t)x;
    if (to_end || (uint16_t)(start + len) < start || start + len > slen) end = slen;
    else end = start + len;
    nursery_reserve_n(1, round4(end - start), 8);
    s = deref_rpop();
    r = str_alloc(end - start);
    memcpy(STRBYTES(r), STRBYTES(s) + start, end - start);
    STRCTX(r, end - start) = CTX(s);
    push(r);
}

/* The INT v as a decimal STRING. */
static obj_t *int_to_str(int64_t v)
{
    char buf[24];
    uint16_t n = 0, i;
    obj_t *o;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    /* 64-bit % and / are ORCA/C's ~UDIV8, ~4,000 cycles each: two per digit
     * made toString 37% of a program that prints numbers (a2min's function
     * profile, eval-okay-big-operations). Almost every number printed fits 16
     * bits, so the narrow loops come first and the wide one runs only above. */
    if (u <= 0xFFFFU) {
        uint16_t w = (uint16_t)u;
        do { buf[n++] = (char)('0' + w % 10); w /= 10; } while (w);
    } else if (u <= 0xFFFFFFFFUL) {
        uint32_t w = (uint32_t)u;
        do { buf[n++] = (char)('0' + (uint16_t)(w % 10)); w /= 10; } while (w);
    } else {
        do { buf[n++] = (char)('0' + (uint16_t)(u % 10)); u /= 10; } while (u);
    }
    if (v < 0) buf[n++] = '-';
    o = str_alloc(n);
    for (i = 0; i < n; ++i) STRBYTES(o)[i] = buf[n - 1 - i];
    return o;
}

static void concat_strings_sep_top(void);

/* The value on top of the stack is replaced by its STRING form: Nix's
 * coerceToString. Runs bytecode.
 *
 * A set coerces through `__toString` (called with the set itself) if it has
 * one, else through `outPath`, and the result is coerced again by the same
 * rules -- which is how `"${drv}"` becomes a store path, and why
 * { __toString = _: true; } is "1" under toString.
 *
 * `how` (vm.h CO_*): CO_MORE is coerceMore -- INTs, BOOLs, null and lists
 * coerce too, a list as its elements joined by spaces (so a throwing element
 * propagates, catchably). CO_PATH makes a path its text, as builtins.toString
 * does; without it a path would be copied into the store, which needs the
 * store (L7), so it is E_UNSUP rather than a string without its context.
 * Without CO_MORE, the rules of `+` and "${}": strings and coercible sets. */
void coerce_top(uint8_t how)
{
    uint16_t b = rsp, n, i, ix, at;
    uint8_t space;
    obj_t *vo, *v = 0, *f;
    vo = force_pop_view(&v);
    if (v->tag == T_ATTRS) {
        if (find_attr(vo, str_tostring, &ix, &f)) {
            push(f);                            /* f self */
            push(vo);
            do_call();
        } else if (find_attr(vo, str_outpath, &ix, &f)) {
            push(f);
        } else {
            type_err();
        }
        coerce_top(how);
        return;
    }
    if (v->tag == T_STRING) { push(vo); return; }
    if (v->tag == T_PATH) {                     /* its text, as a string: no store copy */
        if (!(how & CO_PATH)) err_exit(E_UNSUP);
        rpush(vo);
        nursery_reserve_n(1, round4(AUX16(v)), 8);
        v = deref_rpop();
        vo = str_alloc(AUX16(v));
        memcpy(STRBYTES(vo), STRBYTES(v), AUX16(v));
        push(vo);
        return;
    }
    if (!(how & CO_MORE)) type_err();
    switch (v->tag) {
    case T_INT:    push(int_to_str(INTVAL(v))); return;
    case T_BOOL:   if (BOOLVAL(v)) push(str_one); else push(str_alloc(0)); return;
    case T_NULL:   push(str_alloc(0)); return;
    case T_LIST:   break;
    default:       type_err();
    }
    /* The pieces go on the stack -- each element coerced, and a space after it
     * -- and are joined with nothing between. Nix puts no space after an element
     * that is an EMPTY LIST (its own comment: "not quite correct"), so
     * toString [ [] "a" [] [] "b" [ [] ] "c" ] is "a b  c". Joining with " "
     * gave " a   b  c". */
    n = AUX16(v);
    rpush(vo);                                  /* source */
    at = vsp;
    for (i = 0; i < n; ++i) {
        push(RELEM(b, i, n));
        force_top();
        v = deref(peek());
        space = i + 1 < n && !(v->tag == T_LIST && AUX16(v) == 0);
        coerce_top(how);
        if (space) push(str_space);
    }
    rsp = b;
    v = list_from_stack(vsp - at);
    push(str_alloc(0));                         /* inside the boundary's margin; v is young and nothing collects */
    push(v);
    concat_strings_sep_top();
}

static void prim_tostring(void) { coerce_top(CO_STRING); }

/* hashString type s -> lowercase hex (BYTECODE.md). Only "sha256" -- it is
 * what the card hashes, and what store paths need; md5, sha1 and sha512 are
 * E_UNSUP until something needs them, anything else E_TYPE. Nix forces the
 * type first, then the string, which must BE a string (forceString, not a coercion).
 *
 * The string is hashed where it lives. A mature one stays on the card and
 * HASH_STREAM reads it there: only its 4-byte header crosses the bus. A young
 * one goes through HASH_PUSH. With no card, or the verb masked
 * (checks.verb-economics), sha256.c does it on the CPU. */
/* n bytes as 2n lowercase hex digits. Arithmetic rather than a table: a
 * static table is bank 0 (STATUS.md). */
static void hex_of(const uint8_t *dig, uint16_t n, char *out)
{
    uint16_t i, d;
    for (i = 0; i < 2 * n; ++i) {
        d = i & 1 ? dig[i >> 1] & 15 : dig[i >> 1] >> 4;
        out[i] = (char)(d < 10 ? '0' + d : 'a' - 10 + d);
    }
}

static int is_lit(obj_t *s, const char *lit, uint16_t n)   /* a STRING view */
{
    return AUX16(s) == n && memcmp(STRBYTES(s), lit, n) == 0;
}

static void prim_hashstring(void)
{
    uint8_t dig[32];
    uint16_t n;
    obj_t *t, *so, *s, *r, hdr;
    force_slot(2);                              /* the type, first, as Nix forces it */
    t = deref(vstack[vsp - 2]);
    need(t, T_STRING);
    if (!is_lit(t, "sha256", 6)) {
        if (is_lit(t, "md5", 3) || is_lit(t, "sha1", 4) || is_lit(t, "sha512", 6)) err_exit(E_UNSUP);
        type_err();
    }
    force_top();
    so = peek();
    if (IS_MATURE(so)) {
        card_read_at(PBITS(so), &hdr, 4);
        need(&hdr, T_STRING);
        n = AUX16(&hdr);
        if (!card_sha256_at(PBITS(so) + 4, n, dig)) { s = deref(so); sha256_cpu((uint8_t *)STRBYTES(s), n, dig); }
    } else {
        s = deref(so);
        need(s, T_STRING);
        n = AUX16(s);
        if (!card_sha256((uint8_t *)STRBYTES(s), n, dig)) sha256_cpu((uint8_t *)STRBYTES(s), n, dig);
    }
    vsp -= 2;
    r = str_alloc(64);                          /* 72 bytes: inside the boundary's margin */
    hex_of(dig, 32, STRBYTES(r));
    push(r);
}

/* --- string context (docs/DERIVATION.md "String context") --------------------------- */
obj_t *drv_table;

/* The union of the contexts of the n values at vstack[at..at+n) (anything but
 * a STRING has none): 0; the one list, when a single distinct list is involved
 * -- the usual case, no allocation; else a fresh merged LIST. The merge may
 * collect (every value is on the stack) and turns the ring over between parts,
 * so the caller holds no view across it. */
static obj_t *ctx_join(uint16_t at, uint16_t n)
{
    uint16_t i, j, k, m, i2, base = vsp;
    uint8_t c = 0;
    obj_t *x = 0, *l, *s, *e;
    for (i = 0; i < n; ++i) {
        s = deref_follow(vstack[at + i]);
        if (s->tag != T_STRING || !CTX(s) || CTX(s) == x) continue;
        if (x) break;                           /* a second list: merge */
        x = CTX(s);
    }
    if (i == n) return x;
    for (i = 0; i < n; ++i) {
        windows_maybe();
        s = deref_follow(vstack[at + i]);
        if (s->tag != T_STRING || !(l = CTX(s))) continue;
        m = AUX16(deref(l));
        for (j = 0; j < m; ++j) {               /* insertion into vstack[base..vsp), sorted, once */
            e = list_elem(l, j, m);
            for (k = base; k < vsp && (c = (uint8_t)str_cmp(deref(vstack[k]), deref(e))) == 0; ++k) ;
            if (k < vsp && c == 1) continue;
            push(e);
            for (i2 = vsp - 1; i2 > k; --i2) vstack[i2] = vstack[i2 - 1];
            vstack[k] = e;
        }
    }
    return list_from_stack(vsp - base);
}

/* INTERPOLATE n, and a string `+`, which is the same thing with two parts.
 * Each part is forced and coerced in order, as "${}" coerces -- a set through
 * __toString or outPath, which is how "${drv}" becomes its store path -- then
 * they are concatenated, their contexts united. It was run.c's; it is here
 * because bank 0 is full and every string it builds must carry a context. */
void interpolate(uint16_t n)
{
    uint16_t i, len = 0, at = vsp - n;
    obj_t *o, *s;
    for (i = 0; i < n; ++i) {
        force_slot(n - i);
        if (deref(vstack[at + i])->tag != T_STRING) {
            push(vstack[at + i]);
            coerce_top(CO_PLAIN);
            vstack[at + i] = pop();
        }
    }
    push(ctx_join(at, n));                      /* a root while the result is reserved */
    for (i = 0; i < n; ++i) {
        s = deref(vstack[at + i]);
        need(s, T_STRING);
        if ((uint16_t)(len + AUX16(s)) < len) nomem();
        len += AUX16(s);
    }
    nursery_reserve_n(1, round4(len), 8);       /* the parts are on the stack; the copy loop re-derefs */
    o = str_alloc(len);
    len = 0;
    for (i = 0; i < n; ++i) {
        s = deref(vstack[at + i]);
        memcpy(STRBYTES(o) + len, STRBYTES(s), AUX16(s));
        len += AUX16(s);
    }
    STRCTX(o, len) = pop();
    vsp = at;
    push(o);
}

/* --- derivations (docs/DERIVATION.md) ----------------------------------------------- */
#define DRV_OUT_MAX 8
#define FP_MAX      512         /* the longest output fingerprint: 7 + 209 + 8 + 64 + 12 + 211;
                                   a fixed output's longest, 19 + 128 + 1 + 255, is shorter */
#define NAME_MAX    211         /* StorePath::MaxPathLen */
#define IS_LIT(s, lit) is_lit((s), (lit), sizeof(lit) - 1)

enum { K_OTHER, K_NAME, K_IGNORE, K_STRUCT, K_CA, K_IMPURE, K_ARGS, K_BUILDER, K_SYSTEM,
       K_OUTPUTS, K_HASH, K_HALGO, K_HMODE };

/* The attribute names derivationStrict treats specially, matched against
 * literals, which ORCA/C keeps with the code: no bank 0. nm is a view. */
static uint8_t drv_key(obj_t *nm)
{
    if (IS_LIT(nm, "name"))               return K_NAME;
    if (IS_LIT(nm, "__ignoreNulls"))      return K_IGNORE;
    if (IS_LIT(nm, "__structuredAttrs"))  return K_STRUCT;
    if (IS_LIT(nm, "__contentAddressed")) return K_CA;
    if (IS_LIT(nm, "__impure"))           return K_IMPURE;
    if (IS_LIT(nm, "args"))               return K_ARGS;
    if (IS_LIT(nm, "builder"))            return K_BUILDER;
    if (IS_LIT(nm, "system"))             return K_SYSTEM;
    if (IS_LIT(nm, "outputs"))            return K_OUTPUTS;
    if (IS_LIT(nm, "outputHash"))         return K_HASH;
    if (IS_LIT(nm, "outputHashAlgo"))     return K_HALGO;
    if (IS_LIT(nm, "outputHashMode"))     return K_HMODE;
    return K_OTHER;
}

/* Entry i of the set (an original): its value, and its name in *nm. */
static obj_t *attr_at(obj_t *set, uint16_t i, obj_t **nm)
{
    obj_t *s = deref(set);
    uint16_t n = AUX16(s);
    *nm = ANAMES(s, n)[i];
    return AVALUES(s, n)[i];
}

/* Nix's checkName: 1..211 bytes of [A-Za-z0-9+-._?=], and not ".", "..", nor
 * starting ".-" or "..-". E_TYPE otherwise. */
static void check_name(const char *p, uint16_t n)
{
    uint16_t i;
    char c;
    if (n == 0 || n > NAME_MAX) type_err();
    if (p[0] == '.') {
        if (n == 1 || p[1] == '-') type_err();
        if (p[1] == '.' && (n == 2 || p[2] == '-')) type_err();
    }
    for (i = 0; i < n; ++i) {
        c = p[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              c == '+' || c == '-' || c == '.' || c == '_' || c == '?' || c == '='))
            type_err();
    }
}

/* The store-path name of output ov of the derivation named nmv (both views)
 * into pn: the name for "out", else name-ov. Its length; E_TYPE past 211. */
static uint16_t path_name(char *pn, obj_t *nmv, obj_t *ov)
{
    uint16_t nl = AUX16(nmv), ol = IS_LIT(ov, "out") ? 0 : AUX16(ov);
    if (nl > NAME_MAX || ol > NAME_MAX || (ol && nl + 1 + ol > NAME_MAX)) type_err();
    memcpy(pn, STRBYTES(nmv), nl);
    if (!ol) return nl;
    pn[nl] = '-';
    memcpy(pn + nl + 1, STRBYTES(ov), ol);
    return nl + 1 + ol;
}

/* Where phase 1 left everything. Stack indices, never pointers: phase 2
 * reserves, and a collection moves what they point at. */
struct drv {
    uint16_t at, np;            /* the (attribute, coerced string) pairs, vstack[at + 2i] */
    uint16_t ctx;               /* vstack index of the inputs: the union of every context */
    uint16_t on, no;            /* the output names, sorted, vstack[on + i] */
    uint16_t args;              /* rstack index of the coerced args LIST */
    uint16_t name, bld, sys;    /* pair indices */
    uint16_t nin;               /* elements in the inputs list */
    uint8_t masked;             /* output paths as "" (hashDerivationModulo) */
    uint8_t replaced;           /* input .drv paths as their modulo hashes */
    uint16_t fixed;             /* pair index of outputHash; 0xFFFF: not fixed-output */
    uint8_t fmeth, falgo;       /* its method (FM_*) and algorithm (HA_*) */
    uint8_t fh[64];             /* its hash, ha_size(falgo) bytes */
    obj_t *path[DRV_OUT_MAX];   /* phase 2: the output paths, young, and nothing collects */
};
#define PAIR_KEY(d, i) vstack[(d)->at + 2 * (i)]
#define PAIR_VAL(d, i) vstack[(d)->at + 2 * (i) + 1]
#define OUT_NAME(d, i) vstack[(d)->on + (i)]

/* An ATerm is written through a sink. A null p only counts. */
struct sink { uint8_t *p; uint16_t n, cap; };

static void put(struct sink *k, const char *s, uint16_t m)
{
    if ((uint16_t)(k->n + m) < k->n || k->n + m > k->cap) nomem();
    if (k->p) memcpy(k->p + k->n, s, m);
    k->n += m;
}

/* The STRING o (an original) in quotes: escaped as Nix's printString does,
 * or with esc 0 as its printUnquotedString does -- not at all. The ring may
 * turn over first, because no caller holds a view across this (DERIVATION.md
 * "Views"): a turnover here is as safe as at a bytecode boundary. */
static void put_str(struct sink *k, obj_t *o, uint8_t esc)
{
    uint16_t i, j, m;
    char c, e[2];
    const char *s;
    windows_maybe();
    o = deref(o);
    s = STRBYTES(o);
    m = AUX16(o);
    put(k, "\"", 1);
    for (i = j = 0; esc && i < m; ++i) {
        c = s[i];
        if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t') {
            put(k, s + j, i - j);
            e[0] = '\\';
            e[1] = c == '\n' ? 'n' : c == '\r' ? 'r' : c == '\t' ? 't' : c;
            put(k, e, 2);
            j = i + 1;
        }
    }
    put(k, s + j, m - j);
    put(k, "\"", 1);
}

/* --- fixed outputs (DERIVATION.md "Fixed outputs") ---
 * The hash is given, so nothing here computes md5, sha1 or sha512: every hash
 * the rule makes is a SHA-256. The decoders shift by constants only, because a
 * variable shift links ~SHIFTLEFT into bank 0. */
#define HA_NONE   0
#define HA_MD5    1
#define HA_SHA1   2
#define HA_SHA256 3
#define HA_SHA512 4
#define FM_FLAT   0
#define FM_NAR    1
#define IS_BYTES(p, n, lit) ((n) == sizeof(lit) - 1 && memcmp((p), (lit), (n)) == 0)

/* Nix's parseHashAlgoOpt: HA_NONE for a name it does not know, which Nix
 * drops in silence (measured); blake3 is an experimental feature. */
static uint8_t hash_algo(const char *p, uint16_t n)
{
    if (IS_BYTES(p, n, "md5"))    return HA_MD5;
    if (IS_BYTES(p, n, "sha1"))   return HA_SHA1;
    if (IS_BYTES(p, n, "sha256")) return HA_SHA256;
    if (IS_BYTES(p, n, "sha512")) return HA_SHA512;
    if (IS_BYTES(p, n, "blake3")) err_exit(E_UNSUP);
    return HA_NONE;
}

static uint16_t ha_size(uint8_t a)
{
    return a == HA_MD5 ? 16 : a == HA_SHA1 ? 20 : a == HA_SHA256 ? 32 : 64;
}

/* outputHashMode (a STRING view), as Nix's handleHashMode reads it: text and
 * git are experimental features. */
static uint8_t hash_mode(obj_t *s)
{
    if (IS_LIT(s, "flat")) return FM_FLAT;
    if (IS_LIT(s, "nar") || IS_LIT(s, "recursive")) return FM_NAR;
    if (IS_LIT(s, "text") || IS_LIT(s, "git")) err_exit(E_UNSUP);
    type_err();
    return 0;
}

static uint8_t hex_digit(char c)                /* either case; 16 if not one */
{
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
    return 16;
}

static uint8_t b64_digit(char c)                /* 64 if not one */
{
    if (c >= 'A' && c <= 'Z') return (uint8_t)(c - 'A');
    if (c >= 'a' && c <= 'z') return (uint8_t)(c - 'a' + 26);
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0' + 52);
    if (c == '+') return 62;
    if (c == '/') return 63;
    return 64;
}

/* Nix's newHashAllowEmpty: outputHash (the STRING view s) into d->fh. d->falgo
 * holds outputHashAlgo's algorithm (HA_NONE for none) on entry and the hash's
 * on return. E_TYPE wherever Nix rejects the hash. */
static void parse_hash(struct drv *d, obj_t *s)
{
    const char *p = STRBYTES(s);
    uint16_t n = AUX16(s), i, j, sz;
    uint8_t a, sri = 0, v, m, t, c = 0, *h = d->fh;
    memset(h, 0, 64);
    if (n == 0) {                               /* all zeros (Nix warns) */
        if (d->falgo == HA_NONE) type_err();
        return;
    }
    for (i = 0; i < n && p[i] != ':'; ++i) ;
    if (i == n) {
        for (i = 0; i < n && p[i] != '-'; ++i) ;
        sri = i < n;
    }
    if (i < n) {                                /* "<algo>:", or SRI's "<algo>-" */
        if ((a = hash_algo(p, i)) == HA_NONE) type_err();
        if (d->falgo != HA_NONE && d->falgo != a) type_err();
        p += i + 1;
        n -= i + 1;
    } else if ((a = d->falgo) == HA_NONE) {
        type_err();                             /* "does not include a type" */
    }
    d->falgo = a;
    sz = ha_size(a);
    if (!sri && n == 2 * sz) {                  /* base16 */
        for (i = 0; i < n; ++i) {
            if ((v = hex_digit(p[i])) == 16) type_err();
            h[i >> 1] |= i & 1 ? v : (uint8_t)(v << 4);
        }
    } else if (!sri && n == (8 * sz - 1) / 5 + 1) {
        /* nix32, from the end: the j-th digit from the end holds bits 5j..5j+4.
         * Bit by bit, so that a set bit past the last byte is the error Nix
         * makes it, and every shift is by one. */
        for (j = 0, i = 0, m = 1; j < n; ++j) {
            for (v = 0; v < 32 && "0123456789abcdfghijklmnpqrsvwxyz"[v] != p[n - 1 - j]; ++v) ;
            if (v == 32) type_err();
            for (t = 0; t < 5; ++t, v >>= 1) {
                if (v & 1) {
                    if (i == sz) type_err();
                    h[i] |= m;
                }
                if (!(m <<= 1)) { m = 1; ++i; }
            }
        }
    } else if (sri || n == (sz + 2) / 3 * 4) {
        /* Base64: it stops at the first "=", skips newlines, ignores the bits
         * left over, and must give exactly sz bytes. Four digits are three
         * bytes; c holds the bits not yet in one. */
        for (i = j = 0, t = 0; i < n && p[i] != '='; ++i) {
            if (p[i] == '\n') continue;
            if ((v = b64_digit(p[i])) == 64) type_err();
            if (t == 0) c = v;
            else {
                if (j == sz) type_err();
                if (t == 1)      { h[j++] = (uint8_t)(c << 2 | v >> 4); c = v & 15; }
                else if (t == 2) { h[j++] = (uint8_t)(c << 4 | v >> 2); c = v & 3; }
                else               h[j++] = (uint8_t)(c << 6 | v);
            }
            t = (t + 1) & 3;
        }
        if (j != sz) type_err();
    } else {
        type_err();                             /* "has wrong length" */
    }
}

/* "<m><algo>": the ATerm's second output field, "r:sha256", "sha1", ... */
static void put_method_algo(struct sink *k, struct drv *d)
{
    if (d->fmeth == FM_NAR) put(k, "r:", 2);
    if (d->falgo == HA_MD5) put(k, "md5", 3);
    else if (d->falgo == HA_SHA1) put(k, "sha1", 4);
    else if (d->falgo == HA_SHA256) put(k, "sha256", 6);
    else put(k, "sha512", 6);
}

static void put_hex(struct sink *k, const uint8_t *b, uint16_t n)
{
    char x[2];
    uint16_t i;
    for (i = 0; i < n; ++i) {
        hex_of(b + i, 1, x);
        put(k, x, 2);
    }
}

/* "fixed:out:<m><algo>:<hex>:": what makeFixedOutputPath hashes and, followed
 * by the output's path, what a fixed output's modulo hash is of. */
static void put_fixed(struct sink *k, struct drv *d)
{
    put(k, "fixed:out:", 10);
    put_method_algo(k, d);
    put(k, ":", 1);
    put_hex(k, d->fh, ha_size(d->falgo));
    put(k, ":", 1);
}

/* --- the inputs: the elements of the context union ---
 * Sorted by bytes, the "!<o>!<drv>" elements come first ('!' < '/'), then the
 * "/<path>" sources; "=<drv>" was refused. Each input is read through a KEY:
 * the .drv path, its modulo hash from drv_table, or the store path an element
 * refers to (the .drv, or the source). Keys are views, valid until the ring
 * turns over, which only happens where no key is held. */
#define KEY_DRV 0
#define KEY_HEX 1
#define KEY_REF 2

static int bytes_cmp(const char *a, uint16_t na, const char *b, uint16_t nb)
{
    int c = memcmp(a, b, na < nb ? na : nb);
    return c ? c : (int)na - (int)nb;
}

/* The derivation this program made whose .drv path is p[0..n): its modulo
 * hash, a view of a 64-character STRING. E_UNSUP if there is none -- a
 * context the program did not make. */
static obj_t *drv_hex(const char *p, uint16_t n)
{
    obj_t *t = drv_table, *tv, *pv;
    while (t) {
        tv = deref(t);                          /* [drvPath, hex, next] */
        pv = deref(ELEMS(tv)[0]);
        if (AUX16(pv) == n && memcmp(STRBYTES(pv), p, n) == 0) return deref(ELEMS(tv)[1]);
        t = ELEMS(tv)[2];
    }
    err_exit(E_UNSUP);
    return 0;
}

static obj_t *in_elem(struct drv *d, uint16_t j)       /* element j, a view */
{
    return deref(list_elem(vstack[d->ctx], j, d->nin));
}

static const char *in_key(struct drv *d, uint16_t j, uint8_t kind, uint16_t *len)
{
    obj_t *e = in_elem(d, j);
    const char *p = STRBYTES(e);
    uint16_t m = AUX16(e), i;
    if (p[0] != '!') { *len = m; return p; }            /* a source is itself */
    for (i = 1; p[i] != '!'; ++i) ;                     /* checked in phase 1 */
    if (kind == KEY_HEX) { e = drv_hex(p + i + 1, m - i - 1); *len = 64; return STRBYTES(e); }
    *len = m - i - 1;
    return p + i + 1;
}

/* The element whose key is the smallest greater than last's (-1: the
 * smallest of all): a selection sort, one step. Derivation inputs only, unless
 * the key is KEY_REF. Duplicate keys -- two outputs of one input -- come out
 * once. */
static int16_t in_next(struct drv *d, uint8_t kind, int16_t last)
{
    uint16_t j, lj, lb, ll;
    int16_t best = -1;
    const char *pj, *pb, *pl;
    windows_maybe();
    for (j = 0; j < d->nin; ++j) {
        if (kind != KEY_REF && STRBYTES(in_elem(d, j))[0] != '!') continue;
        pj = in_key(d, j, kind, &lj);
        if (last >= 0) { pl = in_key(d, (uint16_t)last, kind, &ll); if (bytes_cmp(pj, lj, pl, ll) <= 0) continue; }
        if (best >= 0) { pb = in_key(d, (uint16_t)best, kind, &lb); if (bytes_cmp(pj, lj, pb, lb) >= 0) continue; }
        best = (int16_t)j;
    }
    return best;
}

/* [("<drv>",["<o>",...]),...] by .drv path, or with each .drv replaced by its
 * modulo hash and ordered by that (hashDerivationModulo). */
static void put_input_drvs(struct sink *k, struct drv *d)
{
    uint8_t kind = d->replaced ? KEY_HEX : KEY_DRV, fo, first = 1;
    int16_t e = -1;
    uint16_t j, i, lk, lj;
    const char *pk, *pj, *o;
    while ((e = in_next(d, kind, e)) >= 0) {
        if (!first) put(k, ",", 1);
        first = 0;
        pk = in_key(d, (uint16_t)e, kind, &lk);
        put(k, "(\"", 2);
        put(k, pk, lk);
        put(k, "\",[", 3);
        for (j = 0, fo = 1; j < d->nin; ++j) {
            o = STRBYTES(in_elem(d, j));
            if (o[0] != '!') continue;
            pk = in_key(d, (uint16_t)e, kind, &lk);
            pj = in_key(d, j, kind, &lj);
            if (bytes_cmp(pk, lk, pj, lj) != 0) continue;
            o = STRBYTES(in_elem(d, j));        /* "!<o>!..." */
            for (i = 1; o[i] != '!'; ++i) ;
            if (!fo) put(k, ",", 1);
            fo = 0;
            put(k, "\"", 1);
            put(k, o + 1, i - 1);
            put(k, "\"", 1);
        }
        put(k, "])", 2);
    }
}

/* ["<src>",...]: the plain paths, already in order. */
static void put_input_srcs(struct sink *k, struct drv *d)
{
    uint16_t j, first = 1;
    for (j = 0; j < d->nin; ++j) {
        if (STRBYTES(in_elem(d, j))[0] == '!') continue;
        if (!first) put(k, ",", 1);
        first = 0;
        put_str(k, list_elem(vstack[d->ctx], j, d->nin), 0);
    }
}

/* ":<ref>" for every store path the .drv refers to, sorted, once each: the
 * type of its fingerprint is "text" followed by these. */
static void put_refs(struct sink *k, struct drv *d)
{
    int16_t e = -1;
    uint16_t l;
    const char *p;
    while ((e = in_next(d, KEY_REF, e)) >= 0) {
        p = in_key(d, (uint16_t)e, KEY_REF, &l);
        put(k, ":", 1);
        put(k, p, l);
    }
}

/* Derive([outputs],[inputs],[sources],system,builder,[args],[env]). */
static void aterm(struct sink *k, struct drv *d)
{
    uint16_t i, o, n, c;
    put(k, "Derive([", 8);
    for (o = 0; o < d->no; ++o) {
        if (o) put(k, ",", 1);
        put(k, "(", 1);
        put_str(k, OUT_NAME(d, o), 0);
        put(k, ",", 1);
        if (d->masked) put(k, "\"\"", 2); else put_str(k, d->path[o], 0);
        if (d->fixed == 0xFFFF) {
            put(k, ",\"\",\"\")", 7);
        } else {                                /* ,"<m><algo>","<hex>", masked or not */
            put(k, ",\"", 2);
            put_method_algo(k, d);
            put(k, "\",\"", 3);
            put_hex(k, d->fh, ha_size(d->falgo));
            put(k, "\")", 2);
        }
    }
    put(k, "],[", 3);
    put_input_drvs(k, d);
    put(k, "],[", 3);
    put_input_srcs(k, d);
    put(k, "],", 2);
    put_str(k, PAIR_VAL(d, d->sys), 0);         /* unescaped: measured (DERIVATION.md) */
    put(k, ",", 1);
    put_str(k, PAIR_VAL(d, d->bld), 1);
    put(k, ",[", 2);
    n = AUX16(deref(rstack[d->args]));
    for (i = 0; i < n; ++i) {
        if (i) put(k, ",", 1);
        put_str(k, list_elem(rstack[d->args], i, n), 1);
    }
    put(k, "],[", 3);
    /* The environment: the pairs and the outputs merged in key order, an
     * output replacing an attribute of the same name. */
    for (i = o = 0; i < d->np || o < d->no; ) {
        if (i == d->np) c = 2;
        else if (o == d->no) c = 0;
        else c = str_cmp(deref(PAIR_KEY(d, i)), deref(OUT_NAME(d, o)));
        if (i || o) put(k, ",", 1);
        put(k, "(", 1);
        if (c == 0) {
            put_str(k, PAIR_KEY(d, i), 1);
            put(k, ",", 1);
            put_str(k, PAIR_VAL(d, i), 1);
            ++i;
        } else {
            put_str(k, OUT_NAME(d, o), 1);
            put(k, ",", 1);
            if (d->masked) put(k, "\"\"", 2); else put_str(k, d->path[o], 1);
            if (c == 1) ++i;
            ++o;
        }
        put(k, ")", 1);
    }
    put(k, "])", 2);
}

static void sha256_any(const uint8_t *p, uint16_t n, uint8_t *dig)
{
    if (!card_sha256(p, n, dig)) sha256_cpu(p, n, dig);
}

/* makeStorePath: k holds the fingerprint's type ("output:<o>", "text:<refs>");
 * ":sha256:<hex>:/nix/store:<pn>" is appended and the whole hashed, the card's
 * P3 being the hash part (nix32_fold without a card). A young STRING
 * "/nix/store/<32>-<pn>". Nothing collects: the caller reserved. */
static obj_t *store_path(struct sink *k, const uint8_t *dig, const char *pn, uint16_t pl)
{
    char hx[64];
    uint8_t h[32];
    obj_t *r;
    put(k, ":sha256:", 8);
    hex_of(dig, 32, hx);
    put(k, hx, 64);
    put(k, ":/nix/store:", 12);
    put(k, pn, pl);
    r = str_alloc(44 + pl);
    memcpy(STRBYTES(r), "/nix/store/", 11);
    if (!card_storepath(k->p, k->n, (uint8_t *)STRBYTES(r) + 11)) {
        sha256_cpu(k->p, k->n, h);
        nix32_fold(h, STRBYTES(r) + 11);
    }
    STRBYTES(r)[43] = '-';
    memcpy(STRBYTES(r) + 44, pn, pl);
    return r;
}

/* A young one-element context, a LIST holding the STRING a ++ b: the results'
 * "=<drv>" and "!<o>!<drv>". Nothing collects: the caller reserved. */
static obj_t *ctx_one(const char *a, uint16_t na, const char *b, uint16_t nb)
{
    obj_t *e = str_alloc(na + nb), *l = list_alloc(1);
    memcpy(STRBYTES(e), a, na);
    memcpy(STRBYTES(e) + na, b, nb);
    ELEMS(l)[0] = e;
    return l;
}

/* Output names compare as byte strings. */
static int tok_cmp(const char *p, uint16_t a, uint16_t an, uint16_t b, uint16_t bn)
{
    return bytes_cmp(p + a, an, p + b, bn);
}

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/* derivationStrict attrs -> { drvPath; <output> = path; ... } (DERIVATION.md). */
static void prim_derivationstrict(void)
{
    struct drv d;
    struct sink k;
    struct { uint16_t off, len; } tok[DRV_OUT_MAX];
    uint16_t b = rsp, n, i, j, m, key, pl, nl, dl, cnt = 0;
    uint16_t ixn = 0xFFFF, ixs = 0xFFFF, ixi = 0xFFFF, outs = 0xFFFF;
    uint8_t ignore = 0, hasdrv = 0, dig[32];
    uint32_t need, cap, grow = 0;
    char pn[NAME_MAX + 1];
    obj_t *so, *s, *v, *vo, *nm, *r, *e;
    const char *p;

    s = force_pop_as(T_ATTRS, &so);
    n = AUX16(s);
    rpush(so);                                  /* b: the set */
    rpush(list_alloc(0));                       /* b+1: the coerced args */
    d.args = b + 1;

    /* --- phase 1: evaluate. `name` first, then __structuredAttrs and
     * __ignoreNulls, as Nix forces them; then every attribute in name order. */
    for (i = 0; i < n; ++i) {
        windows_maybe();                        /* no view is held here */
        attr_at(rstack[b], i, &nm);
        key = drv_key(deref(nm));
        if (key == K_NAME) ixn = i; else if (key == K_STRUCT) ixs = i; else if (key == K_IGNORE) ixi = i;
    }
    if (ixn == 0xFFFF) err_exit(E_NOATTR);
    push(attr_at(rstack[b], ixn, &nm));
    v = force_pop_as(T_STRING, &vo);            /* forced, not coerced */
    if (CTX(v)) type_err();                     /* and without a context: forceStringNoCtx */
    if (ixs != 0xFFFF) {
        push(attr_at(rstack[b], ixs, &nm));
        v = force_pop_as(T_BOOL, &vo);
        if (BOOLVAL(v)) err_exit(E_UNSUP);
    }
    if (ixi != 0xFFFF) {
        push(attr_at(rstack[b], ixi, &nm));
        v = force_pop_as(T_BOOL, &vo);
        ignore = BOOLVAL(v);
    }
    d.at = vsp;
    d.np = 0;
    d.name = d.bld = d.sys = d.fixed = 0xFFFF;
    d.fmeth = FM_FLAT;
    d.falgo = HA_NONE;
    for (i = 0; i < n; ++i) {
        windows_maybe();
        v = attr_at(rstack[b], i, &nm);
        key = drv_key(deref(nm));
        if (key == K_IGNORE) continue;
        push(nm);                               /* the pair: the name, */
        push(v);                                /* and the value, to coerce */
        if (ignore) {
            force_top();
            if (deref(peek())->tag == T_NULL) { vsp -= 2; continue; }
        }
        switch (key) {
        case K_CA: case K_IMPURE:               /* consumed even when false (measured) */
            v = force_pop_as(T_BOOL, &vo);
            --vsp;
            if (BOOLVAL(v)) err_exit(E_UNSUP);
            continue;
        case K_ARGS:
            v = force_pop_as(T_LIST, &vo);
            --vsp;
            m = AUX16(v);
            rstack[b + 1] = vo;                 /* the source, rooted while it is coerced */
            for (j = 0; j < m; ++j) {
                push(RELEM(b + 1, j, m));
                coerce_top(CO_DRV);
            }
            rstack[b + 1] = list_from_stack(m);
            continue;
        case K_NAME:    d.name = d.np; break;
        case K_BUILDER: d.bld = d.np; break;
        case K_SYSTEM:  d.sys = d.np; break;
        case K_OUTPUTS: outs = d.np; break;
        case K_HASH:    d.fixed = d.np; break;
        }
        coerce_top(CO_DRV);
        if (key == K_HALGO)                     /* read as soon as coerced, as Nix does */
            { s = deref(peek()); d.falgo = hash_algo(STRBYTES(s), AUX16(s)); }
        else if (key == K_HMODE)
            d.fmeth = hash_mode(deref(peek()));
        ++d.np;
    }
    if (d.bld == 0xFFFF || AUX16(deref(PAIR_VAL(&d, d.bld))) == 0) err_exit(E_NOATTR);
    if (d.sys == 0xFFFF || AUX16(deref(PAIR_VAL(&d, d.sys))) == 0) err_exit(E_NOATTR);

    /* The inputs: the union of every coerced string's context -- the pairs
     * (whose names have none) and the arguments, pushed for the moment. */
    m = AUX16(deref(rstack[b + 1]));
    for (i = 0; i < m; ++i) push(list_elem(rstack[b + 1], i, m));
    v = ctx_join(d.at, 2 * d.np + m);
    vsp -= m;
    d.ctx = vsp;
    push(v);
    d.nin = v ? AUX16(deref(v)) : 0;
    for (i = 0; i < d.nin; ++i) {
        windows_maybe();
        e = in_elem(&d, i);
        p = STRBYTES(e);
        m = AUX16(e);
        if (p[0] == '!') {                      /* "!<o>!<drv>": an output of a derivation */
            for (j = 1; j < m && p[j] != '!'; ++j) ;
            if (j == 1 || j + 1 >= m) err_exit(E_UNSUP);
            drv_hex(p + j + 1, m - j - 1);      /* one this program made, or E_UNSUP */
            hasdrv = 1;
        } else if (p[0] != '/') {
            err_exit(E_UNSUP);                  /* "=<drv>": its closure (DERIVATION.md) */
        }
    }

    /* The outputs: `outputs` split on whitespace, sorted, or just "out". */
    d.on = vsp;
    if (outs == 0xFFFF) {
        r = str_alloc(3);                       /* inside the boundary's margin */
        memcpy(STRBYTES(r), "out", 3);
        push(r);
        d.no = 1;
    } else {
        s = deref(PAIR_VAL(&d, outs));
        p = STRBYTES(s);
        m = AUX16(s);
        d.no = 0;
        for (i = 0; ; ) {
            while (i < m && is_ws(p[i])) ++i;
            if (i == m) break;
            j = i;
            while (i < m && !is_ws(p[i])) ++i;
            if (d.no == DRV_OUT_MAX) err_exit(E_UNSUP);
            tok[d.no].off = j;
            tok[d.no].len = i - j;
            ++d.no;
        }
        if (d.no == 0) type_err();              /* "an empty set of outputs" */
        for (i = 1; i < d.no; ++i)              /* insertion sort */
            for (j = i; j && tok_cmp(p, tok[j - 1].off, tok[j - 1].len, tok[j].off, tok[j].len) > 0; --j) {
                m = tok[j].off; tok[j].off = tok[j - 1].off; tok[j - 1].off = m;   /* not a struct copy: */
                m = tok[j].len; tok[j].len = tok[j - 1].len; tok[j - 1].len = m;   /* that links ~MOVE into bank 0 */
            }
        for (i = 0, need = 0; i < d.no; ++i) {
            if (tok[i].len == 7 && memcmp(p + tok[i].off, "drvPath", 7) == 0) type_err();
            if (i && tok_cmp(p, tok[i - 1].off, tok[i - 1].len, tok[i].off, tok[i].len) == 0) type_err();
            need += round4(tok[i].len) + 8;
        }
        if (need > NURSERY_END) nomem();
        nursery_reserve((uint16_t)need);
        s = deref(PAIR_VAL(&d, outs));
        for (i = 0; i < d.no; ++i) {
            r = str_alloc(tok[i].len);
            memcpy(STRBYTES(r), STRBYTES(s) + tok[i].off, tok[i].len);
            push(r);
        }
    }

    /* Names: `name` must not end in .drv; name.drv and each output's path
     * name must be store-path names. */
    s = deref(PAIR_VAL(&d, d.name));
    nl = AUX16(s);
    check_name(STRBYTES(s), nl);                /* first: it bounds nl */
    if (nl + 4 > NAME_MAX) type_err();          /* name.drv */
    if (nl >= 4 && memcmp(STRBYTES(s) + nl - 4, ".drv", 4) == 0) type_err();
    dl = 44 + nl + 4;                           /* the .drv's path */
    if (d.fixed != 0xFFFF) {                    /* fixed-output: exactly "out", and the hash */
        if (d.no != 1 || !IS_LIT(deref(OUT_NAME(&d, 0)), "out")) type_err();
        parse_hash(&d, deref(PAIR_VAL(&d, d.fixed)));
    }
    /* The reserve. The scratch buffer takes, in turn, the masked ATerm (inputs
     * replaced), each output's fingerprint, the final ATerm (real inputs), the
     * .drv's fingerprint and, with inputs, the final ATerm with them replaced
     * again: as long as the longest. Two measuring passes give the masked
     * lengths both ways; the outputs add two copies of each path. A fixed
     * output has no masked ATerm, but its fields are printed masked or not, so
     * the same sum is its final ATerm's length; its fingerprints fit FP_MAX. */
    d.masked = 1;
    d.replaced = 1;
    k.p = 0; k.n = 0; k.cap = 0xFFFF;
    aterm(&k, &d);
    cap = k.n;
    d.replaced = 0;
    k.n = 0;
    aterm(&k, &d);
    if (k.n > cap) cap = k.n;
    k.n = 0;
    put_refs(&k, &d);
    need = (round4(7) + 8)                      /* "drvPath" */
         + (round4(dl) + 8)                     /* the .drv's path */
         + (round4(1 + dl) + 8 + 8)             /* its context, "=<drv>" in a LIST */
         + (16 + 72)                            /* its drv_table entry and modulo hash */
         + (4 + 12 * (d.no + 1));               /* the result set */
    for (i = 0; i < d.no; ++i) {
        pl = path_name(pn, deref(PAIR_VAL(&d, d.name)), deref(OUT_NAME(&d, i)));
        check_name(pn, pl);
        grow += 2 * (44 + pl);                  /* the path, in the outputs and the environment */
        need += round4(44 + pl) + 8;            /* the path */
        need += round4(2 + AUX16(deref(OUT_NAME(&d, i))) + dl) + 8 + 8;   /* its "!<o>!<drv>" */
    }
    cap += grow;
    if (cap < FP_MAX) cap = FP_MAX;
    if (cap < (uint32_t)k.n + 4 + 8 + 64 + 12 + nl + 4) cap = (uint32_t)k.n + 4 + 8 + 64 + 12 + nl + 4;
    if (cap > NURSERY_END - 16) nomem();        /* round4 is 16 bits */
    need += round4((uint16_t)cap) + 8;
    if (need > NURSERY_END) nomem();
    nursery_reserve((uint16_t)need);

    /* --- phase 2: serialise and hash. No bytecode; nothing moves. */
    r = str_alloc((uint16_t)cap);
    k.p = (uint8_t *)STRBYTES(r);
    k.cap = (uint16_t)cap;
    if (d.fixed != 0xFFFF) {                    /* makeFixedOutputPath: from the hash, never the ATerm */
        pl = path_name(pn, deref(PAIR_VAL(&d, d.name)), deref(OUT_NAME(&d, 0)));
        k.n = 0;
        if (d.falgo == HA_SHA256 && d.fmeth == FM_NAR) {
            put(&k, "source", 6);               /* the hash is the fingerprint's own */
            d.path[0] = store_path(&k, d.fh, pn, pl);
        } else {
            put_fixed(&k, &d);
            sha256_any(k.p, k.n, dig);
            k.n = 0;
            put(&k, "output:out", 10);
            d.path[0] = store_path(&k, dig, pn, pl);
        }
    } else {
        d.masked = 1;
        d.replaced = 1;
        k.n = 0;
        aterm(&k, &d);
        sha256_any(k.p, k.n, dig);              /* hashDerivationModulo */
        for (i = 0; i < d.no; ++i) {
            pl = path_name(pn, deref(PAIR_VAL(&d, d.name)), deref(OUT_NAME(&d, i)));
            s = deref(OUT_NAME(&d, i));
            k.n = 0;
            put(&k, "output:", 7);
            put(&k, STRBYTES(s), AUX16(s));
            d.path[i] = store_path(&k, dig, pn, pl);
        }
    }
    d.masked = 0;
    d.replaced = 0;
    k.n = 0;
    aterm(&k, &d);                              /* final */
    sha256_any(k.p, k.n, dig);
    s = deref(PAIR_VAL(&d, d.name));
    memcpy(pn, STRBYTES(s), nl);
    memcpy(pn + nl, ".drv", 4);
    k.n = 0;
    put(&k, "text", 4);
    put_refs(&k, &d);                           /* "text:<every input .drv and source>" */
    v = store_path(&k, dig, pn, nl + 4);
    if (d.fixed != 0xFFFF) {                    /* a fixed output's modulo hash: the hash and */
        k.n = 0;                                /* the path, whatever the inputs */
        put_fixed(&k, &d);
        put(&k, STRBYTES(d.path[0]), AUX16(d.path[0]));
        sha256_any(k.p, k.n, dig);
    } else if (hasdrv) {                        /* the modulo hash for dependents: */
        d.replaced = 1;                         /* final, but with the inputs replaced */
        k.n = 0;
        aterm(&k, &d);
        sha256_any(k.p, k.n, dig);
    }                                           /* (with none, the final ATerm's) */
    nm = str_alloc(64);
    hex_of(dig, 32, STRBYTES(nm));
    e = list_alloc(3);
    ELEMS(e)[0] = v;
    ELEMS(e)[1] = nm;
    ELEMS(e)[2] = drv_table;
    drv_table = e;
    /* The results' contexts: the .drv as a whole, and each output of it. */
    STRCTX(v, dl) = ctx_one("=", 1, STRBYTES(v), dl);
    for (i = 0; i < d.no; ++i) {
        s = deref(OUT_NAME(&d, i));
        pn[0] = '!';
        memcpy(pn + 1, STRBYTES(s), AUX16(s));  /* an output name is at most 209 bytes */
        pn[1 + AUX16(s)] = '!';
        e = d.path[i];
        STRCTX(e, AUX16(e)) = ctx_one(pn, 2 + AUX16(s), STRBYTES(v), dl);
    }
    r = attrs_alloc(d.no + 1);
    nm = str_alloc(7);
    memcpy(STRBYTES(nm), "drvPath", 7);
    insert_entry(r, d.no + 1, &cnt, nm, v);
    for (i = 0; i < d.no; ++i) insert_entry(r, d.no + 1, &cnt, OUT_NAME(&d, i), d.path[i]);
    vsp = d.at;
    rsp = b;
    push(r);
}

/* Nix's isDerivation: the set (an original) has a `type` that forces to the
 * string "derivation". Runs bytecode. */
static int is_drv(obj_t *set)
{
    uint16_t ix;
    obj_t *t;
    if (!find_attr(set, str_type, &ix, &t)) return 0;
    push(t);
    force_top();
    t = deref_pop();
    return t->tag == T_STRING && IS_LIT(t, "derivation");
}

/* values_equal for two sets, a and b (originals, forced, not the same object).
 * Nix's rule first: if both are derivations and both have an outPath, they
 * are equal iff the outPaths are, whatever else differs -- d == d // { x = 1; }
 * is true. Otherwise name by name and value by value. Runs bytecode. It lives
 * here, not in data.c with the other cases, because bank 0 is full. */
int attrs_equal(obj_t *a, obj_t *b)
{
    uint16_t base = rsp, i, n, ix;
    int r = 1;
    obj_t *sa, *sb, *pa, *pb;
    rpush(a);
    rpush(b);
    if (is_drv(rstack[base]) && is_drv(rstack[base + 1]) &&
        find_attr(rstack[base], str_outpath, &ix, &pa) &&
        find_attr(rstack[base + 1], str_outpath, &ix, &pb)) {
        r = values_equal(pa, pb);               /* find_attr does not collect: pa is still good */
    } else {
        n = AUX16(deref(rstack[base]));
        if (AUX16(deref(rstack[base + 1])) != n) r = 0;
        for (i = 0; r && i < n; ++i) {
            sa = deref(rstack[base]);
            sb = deref(rstack[base + 1]);
            if (!str_eq(deref(ANAMES(sa, n)[i]), deref(ANAMES(sb, n)[i]))) r = 0;
            else if (!values_equal(AVALUES(sa, n)[i], AVALUES(sb, n)[i])) r = 0;
        }
    }
    rsp = base;
    return r;
}

/* concatStringsSep sep list, with the result left on the stack. */
static void concat_strings_sep_top(void)
{
    uint16_t b = rsp, n, i, total = 0, nsep, w = 0, at = 0;
    uint8_t coerce = 0, hasctx = 0;
    obj_t *sep, *l, *sepo, *lo, *e, *r;
    force_two_orig(&sep, &l, &sepo, &lo);
    need(sep, T_STRING); need(l, T_LIST);
    n = AUX16(l);
    rpush(sepo);
    rpush(lo);
    /* Force every element and measure it, stopping at the first that is not a
     * string. One pass, not a separate measuring one: with every element
     * mature, each pass copies each string into the window ring, and three
     * passes over 1,000 strings overran an instruction's 44 KB of ring
     * (eval-okay-big-operations under A2MIN_RESERVE_STRESS). */
    for (i = 0; i < n; ++i) {
        push(RELEM(b + 1, i, n));
        force_top();
        e = deref(pop());
        if (e->tag != T_STRING) { coerce = 1; break; }
        if (CTX(e)) hasctx = 1;
        if ((uint16_t)(total + AUX16(e)) < total) nomem();
        total += AUX16(e);
    }
    if (!coerce && (hasctx || CTX(deref(rstack[b])))) {
        /* A context to unite: the elements go on the stack, as coerced ones
         * would, for ctx_join to see and the copy to read. */
        at = vsp;
        for (i = 0; i < n; ++i) push(RELEM(b + 1, i, n));
        coerce = 2;
    } else if (coerce) {
        total = 0;
        /* Some element coerces (a set via __toString / outPath), so the text
         * to copy is the COERCED value, not the element: keep every coerced
         * string on the value stack, a root, and copy from there. (The first
         * version coerced to measure and then copied the original elements --
         * a set's header bytes as text: "F\x9a,\x9a,plain".) Bounded by the
         * value stack; loud E_NOMEM past it. */
        at = vsp;
        for (i = 0; i < n; ++i) {
            push(RELEM(b + 1, i, n));
            coerce_top(CO_PLAIN);
            e = deref(peek());
            if ((uint16_t)(total + AUX16(e)) < total) nomem();
            total += AUX16(e);
        }
    }
    sepo = rstack[b];
    sep = deref(sepo);
    nsep = AUX16(sep);
    for (i = 1; i < n; ++i) {                   /* n-1 separators */
        if ((uint16_t)(total + nsep) < total) nomem();
        total += nsep;
    }
    if (coerce) {                               /* the elements' contexts and the separator's */
        push(rstack[b]);
        e = ctx_join(at, n + 1);
        --vsp;
    } else {
        e = 0;
    }
    push(e);                                    /* a root across the reserve */
    nursery_reserve_n(1, round4(total), 8);     /* sep, the list, any coerced strings: all rooted */
    sepo = rstack[b];
    lo = rstack[b + 1];
    rsp = b;
    r = str_alloc(total);
    for (i = 0; i < n; ++i) {
        if (i) { sep = deref(sepo); memcpy(STRBYTES(r) + w, STRBYTES(sep), nsep); w += nsep; }
        if (coerce) e = deref_follow(vstack[at + i]);
        else e = deref_follow(list_elem(lo, i, n));   /* not deref(lo): a long mature list re-copied per element overran the window ring */
        memcpy(STRBYTES(r) + w, STRBYTES(e), AUX16(e));
        w += AUX16(e);
    }
    STRCTX(r, total) = pop();
    if (coerce) vsp = at;
    push(r);
}

static void prim_concatstringssep(void) { concat_strings_sep_top(); }

/* baseNameOf / dirOf on STRINGs, with Nix's edge cases. */
static void prim_basenameof(void)
{
    obj_t *s = view_top_text(), *r;       /* stays rooted until the reserve */
    uint16_t n = AUX16(s), last, start;
    const char *p = STRBYTES(s);
    if (n == 0) {
        r = str_alloc(0);
        if (s->tag == T_STRING) STRCTX(r, 0) = CTX(s);
        --vsp;
        push(r);
        return;
    }
    last = n - 1;
    if (last && p[last] == '/') --last;        /* a trailing slash is ignored */
    start = last;
    for (;;) {
        if (p[start] == '/') { ++start; break; }
        if (start == 0) break;
        --start;
    }
    if (nursery_reserve_n(1, round4(last - start + 1), 8)) { s = view_top_text(); p = STRBYTES(s); }
    --vsp;
    r = str_alloc(last - start + 1);
    memcpy(STRBYTES(r), p + start, last - start + 1);
    if (s->tag == T_STRING) STRCTX(r, last - start + 1) = CTX(s);   /* a string's context stays */
    push(r);
}

/* The static STRING st, or with context x a young copy of it that has it. */
static void push_text(obj_t *st, obj_t *x)
{
    obj_t *v, *r;
    if (!x) { push(st); return; }
    v = deref(st);
    r = str_alloc(AUX16(v));                    /* a byte or two: inside the margin */
    memcpy(STRBYTES(r), STRBYTES(v), AUX16(v));
    STRCTX(r, AUX16(v)) = x;
    push(r);
}

static void prim_dirof(void)
{
    obj_t *s = view_top_text(), *r, *x;   /* stays rooted until the reserve */
    uint16_t n = AUX16(s), j;
    const char *p = STRBYTES(s);
    x = s->tag == T_STRING ? CTX(s) : 0;  /* a string's context stays; a path has none */
    if (n == 0) { --vsp; push_text(str_dot, x); return; }
    j = n - 1;
    for (;;) {
        if (p[j] == '/') break;
        if (j == 0) { --vsp; push_text(str_dot, x); return; }
        --j;
    }
    /* A path's directory is a path; a string's is a string. A path always has
     * a slash (it is absolute and canonical), so it never reaches str_dot. */
    if (j == 0) {
        --vsp;
        if (s->tag == T_PATH) { r = path_alloc(1); STRBYTES(r)[0] = '/'; push(r); }
        else push_text(str_slash, x);
        return;
    }
    if (nursery_reserve_n(1, round4(j), 8)) {
        s = view_top_text();
        p = STRBYTES(s);
        x = s->tag == T_STRING ? CTX(s) : 0;    /* the collection may have moved it */
    }
    --vsp;
    r = s->tag == T_PATH ? path_alloc(j) : str_alloc(j);
    memcpy(STRBYTES(r), p, j);
    if (x) STRCTX(r, j) = x;
    push(r);
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
    obj_t *o, *v;
    o = force_pop_view(&v);
    switch (v->tag) {
    case T_INT:    push(str_int); break;
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
    obj_t *o, *v;
    o = force_pop_view(&v);
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
    default:                   err_exit(E_BADOP);
    }
}
