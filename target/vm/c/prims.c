/* prims.c -- the primops (BYTECODE.md "Primops"). Translated from
 * target/vm/prims.s.
 *
 * PRIM id: the arguments are on the value stack, first argument deepest,
 * unforced. Each primop pops what it takes, forces what it needs and pushes one
 * value. Loops that call back into Nix (map, filter, foldl', any, all, ...)
 * keep their objects on the root stack, because do_call may collect.
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
SSTR(str_one, "1");
SSTR(str_dot, ".");
SSTR(str_slash, "/");
SSTR(str_name, "name");
SSTR(str_right, "right");
SSTR(str_wrong, "wrong");
SSTR(str_space, " ");

static obj_t *str_int, *str_bool, *str_null, *str_string, *str_list, *str_set, *str_lambda,
             *str_one, *str_dot, *str_slash, *str_name, *str_right, *str_wrong, *str_space;

void prims_init(void)
{
    str_int    = STATIC_PTR(&str_int_s);
    str_bool   = STATIC_PTR(&str_bool_s);
    str_null   = STATIC_PTR(&str_null_s);
    str_string = STATIC_PTR(&str_string_s);
    str_list   = STATIC_PTR(&str_list_s);
    str_set    = STATIC_PTR(&str_set_s);
    str_lambda = STATIC_PTR(&str_lambda_s);
    str_one    = STATIC_PTR(&str_one_s);
    str_dot    = STATIC_PTR(&str_dot_s);
    str_slash  = STATIC_PTR(&str_slash_s);
    str_name   = STATIC_PTR(&str_name_s);
    str_right  = STATIC_PTR(&str_right_s);
    str_wrong  = STATIC_PTR(&str_wrong_s);
    str_space  = STATIC_PTR(&str_space_s);
}

/* --- shared ---------------------------------------------------------------------------- */
static void push_bool(int v) { push(v ? obj_true : obj_false); }

static obj_t *force_pop(void)
{
    force_top();
    return deref_pop();
}

static obj_t *force_pop_as(uint8_t tag)
{
    obj_t *o = force_pop();
    need(o, tag);
    return o;
}

/* An INT that is an index or a length: 0..65535, else an index error. */
static uint16_t pop_index(void)
{
    obj_t *o = force_pop_as(T_INT);
    int64_t v = INTVAL(o);
    if (v < 0 || v > 0xFFFF) err_exit(E_INDEX);
    return (uint16_t)v;
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

/* The i-th element of the LIST whose pointer is root stack entry r. */
#define RELEM(r, i) (ELEMS(deref(rstack[r]))[i])

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
    obj_t *l = force_pop_as(T_LIST);
    push(new_int(AUX16(l)));
}

static void prim_head(void)
{
    obj_t *l = force_pop_as(T_LIST);
    if (AUX16(l) == 0) err_exit(E_INDEX);
    push(ELEMS(l)[0]);
}

static void prim_tail(void)
{
    obj_t *l = force_pop_as(T_LIST), *o;
    uint16_t n = AUX16(l);
    if (n == 0) err_exit(E_INDEX);
    o = list_alloc(n - 1);
    memcpy(ELEMS(o), ELEMS(l) + 1, 4 * (n - 1));
    push(o);
}

static void prim_elemat(void)                   /* list i */
{
    uint16_t i = pop_index();
    obj_t *l = force_pop_as(T_LIST);
    if (i >= AUX16(l)) err_exit(E_INDEX);
    push(ELEMS(l)[i]);
}

static void prim_map(void)                      /* f list -> a list of (f x) thunks */
{
    obj_t *l = force_pop_as(T_LIST), *f, *o;
    uint16_t n = AUX16(l), i;
    f = pop();
    o = list_alloc(n);
    for (i = 0; i < n; ++i) ELEMS(o)[i] = make_apply(f, ELEMS(l)[i]);
    push(o);
}

static void prim_genlist(void)                  /* f n -> [ (f 0) ... (f (n-1)) ] */
{
    uint16_t n = pop_index(), i;
    obj_t *f = pop(), *o = list_alloc(n);
    for (i = 0; i < n; ++i) ELEMS(o)[i] = make_apply(f, new_int(i));
    push(o);
}

static void prim_filter(void)                   /* f list. Roots: list, f, result */
{
    uint16_t b = rsp, n, i, kept = 0;
    obj_t *l = force_pop_as(T_LIST), *r;
    n = AUX16(l);
    rpush(l);
    rpush(pop());
    rpush(list_alloc(n));
    for (i = 0; i < n; ++i) {
        call_with(rstack[b + 1], RELEM(b, i));
        if (BOOLVAL(force_pop_as(T_BOOL)))
            RELEM(b + 2, kept++) = RELEM(b, i);
    }
    r = deref(rstack[b + 2]);
    SET_HDR(r, T_LIST, kept);
    rsp = b;
    push(r);
}

static void prim_foldl(void)                    /* f z list: the accumulator lives on the value stack */
{
    uint16_t b = rsp, n, i;
    obj_t *l = force_pop_as(T_LIST), *z;
    n = AUX16(l);
    rpush(l);
    z = pop();
    rpush(pop());                               /* f */
    push(z);
    for (i = 0; i < n; ++i) {
        obj_t *acc = pop();
        call_with(rstack[b + 1], acc);          /* g = f acc */
        push(RELEM(b, i));
        do_call();                              /* acc' = g x */
        force_top();                            /* strict accumulator */
    }
    rsp = b;
}

static void prim_concatlists(void)              /* [ [..] [..] ]: force each, sum, copy */
{
    uint16_t b = rsp, n, i, total = 0, w = 0;
    obj_t *l = force_pop_as(T_LIST), *e, *o;
    n = AUX16(l);
    rpush(l);
    for (i = 0; i < n; ++i) {
        push(RELEM(b, i));
        e = force_pop_as(T_LIST);
        if ((uint16_t)(total + AUX16(e)) < total) nomem();
        total += AUX16(e);
    }
    o = list_alloc(total);
    for (i = 0; i < n; ++i) {
        e = deref_follow(RELEM(b, i));
        memcpy(ELEMS(o) + w, ELEMS(e), 4 * AUX16(e));
        w += AUX16(e);
    }
    rsp = b;
    push(o);
}

static void prim_concatmap(void)                /* concatLists (map f list), the map strict */
{
    uint16_t b = rsp, n, i;
    obj_t *l = force_pop_as(T_LIST), *m;
    n = AUX16(l);
    rpush(l);
    rpush(pop());                               /* f */
    rpush(list_alloc(n));                       /* mapped */
    for (i = 0; i < n; ++i) {
        call_with(rstack[b + 1], RELEM(b, i));
        force_top();
        RELEM(b + 2, i) = pop();
    }
    m = rstack[b + 2];
    rsp = b;
    push(m);
    prim_concatlists();
}

static void prim_elem(void)                     /* x list */
{
    uint16_t b = rsp, n, i;
    obj_t *l = force_pop_as(T_LIST);
    int found = 0;
    n = AUX16(l);
    rpush(l);
    rpush(pop());                               /* x */
    for (i = 0; i < n; ++i)
        if (values_equal(RELEM(b, i), rstack[b + 1])) { found = 1; break; }
    rsp = b;
    push_bool(found);
}

static void any_all(int want)                   /* f list; `want` decides early */
{
    uint16_t b = rsp, n, i;
    obj_t *l = force_pop_as(T_LIST);
    int r = !want;
    n = AUX16(l);
    rpush(l);
    rpush(pop());                               /* f */
    for (i = 0; i < n; ++i) {
        call_with(rstack[b + 1], RELEM(b, i));
        if ((BOOLVAL(force_pop_as(T_BOOL)) != 0) == want) { r = want; break; }
    }
    rsp = b;
    push_bool(r);
}

/* --- attribute sets ---------------------------------------------------------------------- */
static void prim_attrnames(void)
{
    obj_t *s = force_pop_as(T_ATTRS), *o;
    uint16_t n = AUX16(s);
    o = list_alloc(n);
    memcpy(ELEMS(o), ANAMES(s, n), 4 * n);
    push(o);
}

static void prim_attrvalues(void)
{
    obj_t *s = force_pop_as(T_ATTRS), *o;
    uint16_t n = AUX16(s);
    o = list_alloc(n);
    memcpy(ELEMS(o), AVALUES(s, n), 4 * n);
    push(o);
}

/* hasAttr name set / getAttr name set. Nix forces the NAME first, so a throwing
 * name is caught before a non-set second argument is a type error. */
static void force_name_then_set(obj_t **name, obj_t **set)
{
    force_slot(2);
    *set = force_pop_as(T_ATTRS);
    *name = deref_pop();
    need(*name, T_STRING);
}

static void prim_hasattr(void)
{
    obj_t *name, *set;
    uint16_t ix;
    force_name_then_set(&name, &set);
    push_bool(find_attr(set, name, &ix));
}

static void prim_getattr(void)
{
    obj_t *name, *set;
    uint16_t ix;
    force_name_then_set(&name, &set);
    if (!find_attr(set, name, &ix)) err_exit(E_NOATTR);
    push(AVALUES(set, AUX16(set))[ix]);
}

static void prim_intersectattrs(void)           /* names of a that are in b, with b's values */
{
    obj_t *a, *b, *o;
    uint16_t na, nb, i, j, k = 0;
    force_two(&a, &b);
    need(a, T_ATTRS); need(b, T_ATTRS);
    na = AUX16(a); nb = AUX16(b);
    for (i = 0; i < na; ++i)
        if (find_attr(b, deref(ANAMES(a, na)[i]), &j)) ++k;
    o = attrs_alloc(k);
    for (i = 0, k = 0; i < na; ++i)
        if (find_attr(b, deref(ANAMES(a, na)[i]), &j)) { copy_entry(o, AUX16(o), k, b, nb, j); ++k; }
    push(o);
}

/* Is the STRING `name` among the (forced) elements of LIST l? */
static int name_in_list(obj_t *name, obj_t *l)
{
    uint16_t n = AUX16(l), j;
    for (j = 0; j < n; ++j)
        if (str_eq(name, deref_follow(ELEMS(l)[j]))) return 1;
    return 0;
}

static void prim_removeattrs(void)              /* set [ names ]: keep entries not in the list */
{
    uint16_t b = rsp, ns, nl, i, k = 0;
    obj_t *s, *l, *o;
    force_two(&s, &l);
    need(s, T_ATTRS); need(l, T_LIST);
    nl = AUX16(l);
    rpush(s);
    rpush(l);
    for (i = 0; i < nl; ++i) {                  /* force every name */
        push(RELEM(b + 1, i));
        force_pop_as(T_STRING);
    }
    s = deref(rstack[b]);
    l = deref(rstack[b + 1]);
    rsp = b;
    ns = AUX16(s);
    for (i = 0; i < ns; ++i)
        if (!name_in_list(deref(ANAMES(s, ns)[i]), l)) ++k;
    o = attrs_alloc(k);
    for (i = 0, k = 0; i < ns; ++i)
        if (!name_in_list(deref(ANAMES(s, ns)[i]), l)) { copy_entry(o, AUX16(o), k, s, ns, i); ++k; }
    push(o);
}

/* Sorted insertion into the ATTRS `set` of capacity `cap` holding *cnt
 * entries; a duplicate name is dropped. Nothing here runs bytecode. */
static void insert_entry(obj_t *set, uint16_t cap, uint16_t *cnt, obj_t *name, obj_t *value)
{
    uint16_t j, k, c;
    for (j = 0; j < *cnt; ++j) {
        c = str_cmp(deref(ANAMES(set, cap)[j]), name);
        if (c == 1) return;                     /* first occurrence wins */
        if (c == 2) break;                      /* existing > new: insert here */
    }
    for (k = *cnt; k > j; --k) {
        HASHES(set)[k] = HASHES(set)[k - 1];
        ANAMES(set, cap)[k] = ANAMES(set, cap)[k - 1];
        AVALUES(set, cap)[k] = AVALUES(set, cap)[k - 1];
    }
    HASHES(set)[j] = hash_str(name);
    ANAMES(set, cap)[j] = name;
    AVALUES(set, cap)[j] = value;
    ++*cnt;
}

static void prim_listtoattrs(void)              /* [ { name; value; } ... ] */
{
    uint16_t b = rsp, n, i, cnt = 0, ix;
    obj_t *l = force_pop_as(T_LIST), *e, *set;
    n = AUX16(l);
    rpush(l);
    rpush(attrs_alloc(n));                      /* capacity n; column offsets use n */
    for (i = 0; i < n; ++i) {
        push(RELEM(b, i));
        e = force_pop_as(T_ATTRS);
        rpush(e);                               /* b+2: the element */
        if (!find_attr(e, str_name, &ix)) err_exit(E_NOATTR);
        push(AVALUES(e, AUX16(e))[ix]);
        rpush(force_pop_as(T_STRING));          /* b+3: the name */
        e = deref(rstack[b + 2]);
        if (!find_attr(e, str_value, &ix)) err_exit(E_NOATTR);
        insert_entry(deref(rstack[b + 1]), n, &cnt, rstack[b + 3], AVALUES(e, AUX16(e))[ix]);
        rsp = b + 2;
    }
    set = deref(rstack[b + 1]);
    rsp = b;
    if (cnt != n) {                             /* duplicates were dropped: compact */
        obj_t *o = attrs_alloc(cnt);
        for (i = 0; i < cnt; ++i) copy_entry(o, cnt, i, set, n, i);
        set = o;
    }
    push(set);
}

static void prim_partition(void)                /* f list -> { right; wrong; } */
{
    uint16_t b = rsp, n, i, nr = 0, nw = 0;
    obj_t *l = force_pop_as(T_LIST), *o, *r, *w;
    n = AUX16(l);
    rpush(l);
    rpush(pop());                               /* f */
    rpush(list_alloc(n));                       /* right */
    rpush(list_alloc(n));                       /* wrong */
    for (i = 0; i < n; ++i) {
        call_with(rstack[b + 1], RELEM(b, i));
        if (BOOLVAL(force_pop_as(T_BOOL))) RELEM(b + 2, nr++) = RELEM(b, i);
        else                                   RELEM(b + 3, nw++) = RELEM(b, i);
    }
    r = deref(rstack[b + 2]); SET_HDR(r, T_LIST, nr);
    w = deref(rstack[b + 3]); SET_HDR(w, T_LIST, nw);
    rsp = b;
    o = attrs_alloc(2);
    attrs_put(o, 2, 0, str_right, r);
    attrs_put(o, 2, 1, str_wrong, w);
    push(o);
}

/* --- strings ---------------------------------------------------------------------------- */
static void prim_stringlength(void)
{
    obj_t *s = force_pop_as(T_STRING);
    push(new_int(AUX16(s)));
}

static void prim_substring(void)                /* start len s */
{
    uint16_t b = rsp, start, len, slen, end;
    int to_end = 0;
    obj_t *s = force_pop_as(T_STRING), *v, *o;
    int64_t x;
    rpush(s);
    v = force_pop_as(T_INT);                    /* len: negative means "to the end" */
    x = INTVAL(v);
    if (x < 0 || x > 0xFFFF) to_end = 1; else len = (uint16_t)x;
    v = force_pop_as(T_INT);                    /* start */
    x = INTVAL(v);
    if (x < 0) err_exit(E_INDEX);
    s = deref_rpop();
    slen = AUX16(s);
    if (x > 0xFFFF || (uint16_t)x >= slen) { push(str_alloc(0)); return; }
    start = (uint16_t)x;
    if (to_end || (uint16_t)(start + len) < start || start + len > slen) end = slen;
    else end = start + len;
    o = str_alloc(end - start);
    memcpy(STRBYTES(o), STRBYTES(s) + start, end - start);
    push(o);
    (void)b;
}

/* The INT v as a decimal STRING. */
static obj_t *int_to_str(int64_t v)
{
    char buf[24];
    uint16_t n = 0, i;
    obj_t *o;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    do { buf[n++] = (char)('0' + (uint16_t)(u % 10)); u /= 10; } while (u);
    if (v < 0) buf[n++] = '-';
    o = str_alloc(n);
    for (i = 0; i < n; ++i) STRBYTES(o)[i] = buf[n - 1 - i];
    return o;
}

static void concat_strings_sep_top(void);

/* The value on top of the stack is replaced by its STRING form. A list is its
 * elements' strings joined by spaces (Nix's coercion), so a throwing element
 * propagates -- catchably -- through it. Runs bytecode. */
static void tostring_top(void)
{
    uint16_t b = rsp, n, i;
    obj_t *v = force_pop(), *m;
    switch (v->tag) {
    case T_STRING: push(v); return;
    case T_INT:    push(int_to_str(INTVAL(v))); return;
    case T_BOOL:   if (BOOLVAL(v)) push(str_one); else push(str_alloc(0)); return;
    case T_NULL:   push(str_alloc(0)); return;
    case T_LIST:   break;
    default:       type_err();
    }
    n = AUX16(v);
    rpush(v);                                   /* source */
    rpush(list_alloc(n));                       /* the converted elements */
    for (i = 0; i < n; ++i) {
        push(RELEM(b, i));
        tostring_top();
        RELEM(b + 1, i) = pop();
    }
    m = rstack[b + 1];
    rsp = b;
    push(str_space);
    push(m);
    concat_strings_sep_top();
}

static void prim_tostring(void) { tostring_top(); }

/* concatStringsSep sep list, with the result left on the stack. */
static void concat_strings_sep_top(void)
{
    uint16_t b = rsp, n, i, total = 0, nsep, w = 0;
    obj_t *sep, *l, *e, *o;
    force_two(&sep, &l);
    need(sep, T_STRING); need(l, T_LIST);
    n = AUX16(l);
    rpush(sep);
    rpush(l);
    for (i = 0; i < n; ++i) {
        push(RELEM(b + 1, i));
        e = force_pop_as(T_STRING);
        if ((uint16_t)(total + AUX16(e)) < total) nomem();
        total += AUX16(e);
    }
    sep = deref(rstack[b]);
    l = deref(rstack[b + 1]);
    rsp = b;
    nsep = AUX16(sep);
    for (i = 1; i < n; ++i) {                   /* n-1 separators */
        if ((uint16_t)(total + nsep) < total) nomem();
        total += nsep;
    }
    o = str_alloc(total);
    for (i = 0; i < n; ++i) {
        if (i) { memcpy(STRBYTES(o) + w, STRBYTES(sep), nsep); w += nsep; }
        e = deref_follow(ELEMS(l)[i]);
        memcpy(STRBYTES(o) + w, STRBYTES(e), AUX16(e));
        w += AUX16(e);
    }
    push(o);
}

static void prim_concatstringssep(void) { concat_strings_sep_top(); }

/* baseNameOf / dirOf on STRINGs, with Nix's edge cases. */
static void prim_basenameof(void)
{
    obj_t *s = force_pop_as(T_STRING), *o;
    uint16_t n = AUX16(s), last, start;
    const char *p = STRBYTES(s);
    if (n == 0) { push(str_alloc(0)); return; }
    last = n - 1;
    if (last && p[last] == '/') --last;        /* a trailing slash is ignored */
    start = last;
    for (;;) {
        if (p[start] == '/') { ++start; break; }
        if (start == 0) break;
        --start;
    }
    o = str_alloc(last - start + 1);
    memcpy(STRBYTES(o), p + start, last - start + 1);
    push(o);
}

static void prim_dirof(void)
{
    obj_t *s = force_pop_as(T_STRING), *o;
    uint16_t n = AUX16(s), j;
    const char *p = STRBYTES(s);
    if (n == 0) { push(str_dot); return; }
    j = n - 1;
    for (;;) {
        if (p[j] == '/') break;
        if (j == 0) { push(str_dot); return; }
        --j;
    }
    if (j == 0) { push(str_slash); return; }
    o = str_alloc(j);
    memcpy(STRBYTES(o), p, j);
    push(o);
}

/* --- errors, forcing, types --------------------------------------------------------------- */
static void throw_exit(uint8_t code)
{
    obj_t *s = force_pop_as(T_STRING);
    *(obj_t **)RESULT_ADDR = s;                 /* the message travels in RESULT */
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
    obj_t *v = force_pop();
    switch (v->tag) {
    case T_INT:    push(str_int); break;
    case T_BOOL:   push(str_bool); break;
    case T_NULL:   push(str_null); break;
    case T_STRING: push(str_string); break;
    case T_LIST:   push(str_list); break;
    case T_ATTRS:  push(str_set); break;
    case T_LAMBDA: push(str_lambda); break;
    default:       type_err();
    }
}

static void is_tag(uint8_t tag)
{
    obj_t *v = force_pop();
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
/* The arithmetic and comparison primops share the operators' code; run.c
 * exposes them for that. */
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
    default:                   err_exit(E_BADOP);
    }
}
