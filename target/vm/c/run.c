/* run.c -- the dispatch loop, environments, calls, closures and forcing.
 * Translated from target/vm/vm.s and the with/select parts of data.s.
 *
 * Each handler leaves its result on the value stack and returns to the loop;
 * RETURN returns from run(). FORCE recurses through run(), so the C stack
 * carries the frames exactly as the 65816 stack carried the assembly's.
 */
#pragma noroot
#pragma optimize 15

#include "vm.h"

const uint8_t *pc;
obj_t **ctab;
obj_t *env;
obj_t *fstack[FSTACK_N];
uint16_t fstack_n;

/* --- operand fetch ------------------------------------------------------------------ */
#define fetch8()  (*pc++)
#define fetch16() (pc += 2, (uint16_t)(pc[-2] | ((uint16_t)pc[-1] << 8)))

/* --- the C stack -------------------------------------------------------------------- */
uint16_t stack_ptr(void)
{
    uint16_t s;
    asm {
        tsc
        sta s
    }
    return s;
}

void stack_check(void)
{
    if (stack_ptr() < STACK_MIN) nomem();
}

/* --- forcing ------------------------------------------------------------------------- */
/* The one pointer an in-place update of a thunk with the value v (view vv)
 * writes, and its offset: the value, when the thunk becomes an INDIRECT to it;
 * the environment, when a closure is copied over the thunk; 0 when the value
 * is copied and holds no pointer. Mirrors force_thunk's switch below. */
static obj_t *update_ptr(obj_t *vv, obj_t *v, uint16_t *off)
{
    switch (vv->tag) {
    case T_INT: case T_FLOAT: case T_BOOL: case T_NULL: return 0;
    case T_LAMBDA: case T_THUNK: *off = 8; return CL_ENV(vv);
    default: *off = 4; return v;
    }
}

/* The thunk on top of the stack: blackhole it, remember it on FSTACK (a caught
 * throw un-blackholes it), run its body with its captured environment, then
 * update it in place with the value -- a value of 12 bytes or fewer overwrites
 * the thunk; anything larger makes the thunk an INDIRECT to it. The pointer on
 * top is the thunk's afterwards (it may have moved during the body). */
static void force_thunk(void)
{
    obj_t *th = peek(), *tv, *v, *vv, *y;
    uint16_t yoff;
    const uint8_t *spc = pc;
    ++ST_FORCES;
    obj_t **sctab = ctab;
    uint32_t buf[3];
    stack_check();
    tv = deref(th);
    if (fstack_n >= FSTACK_N) nomem();
    fstack[fstack_n++] = th;
    rpush(env);
    pc = IMAGE_PC(CL_CODE(tv));
    ctab = CTAB_AT(CL_CTAB(tv));
    env = CL_ENV(tv);
    store_byte(th, 0, T_BLACKHOLE);             /* re-entry during evaluation is infinite recursion */
    run();                                      /* leaves the value above the thunk */
    env = rpop();
    ctab = sctab;
    pc = spc;
    th = fstack[--fstack_n];                    /* the thunk, wherever it is now */
    v = pop();
    if (IS_MATURE(th)) {
        /* The update goes through the bus: the write barrier. It can put one
         * pointer on the card -- the value itself, for an INDIRECT, or the
         * environment of a closure copied in place -- and if that pointer is
         * young the field goes in the remembered set. A mature v can carry a
         * young environment too (it may itself be a thunk updated this way), so
         * the test is on what is written, not on v. Only a full set collects. */
        vv = deref(v);
        y = update_ptr(vv, v, &yoff);
        if (IS_YOUNG(y) && rs_full()) {
            ++ST_BARRIER; push(v); gc_collect(); v = pop();
            vv = deref(v);                      /* now nothing it carries is young */
            y = update_ptr(vv, v, &yoff);
        }
        switch (vv->tag) {
        case T_INT: case T_FLOAT: case T_LAMBDA: case T_THUNK:
            memcpy(buf, vv, 12); card_write_at(PBITS(th), buf, 12); break;   /* gcsafe: mature -- th is on the card */
        case T_BOOL: case T_NULL:
            memcpy(buf, vv, 8); card_write_at(PBITS(th), buf, 8); break;
        default:
            SET_HDR((obj_t *)buf, T_INDIRECT, 0); buf[1] = PBITS(v);
            card_write_at(PBITS(th), buf, 8);
        }
        if (IS_YOUNG(y)) remember(th, yoff);
        windows_invalidate(th);
    } else {
        vv = deref(v);
        switch (vv->tag) {
        case T_INT: case T_FLOAT: case T_LAMBDA: case T_THUNK:
            ((uint32_t *)th)[2] = ((uint32_t *)vv)[2];
            /* fall through */
        case T_BOOL: case T_NULL:
            ((uint32_t *)th)[1] = ((uint32_t *)vv)[1];
            ((uint32_t *)th)[0] = ((uint32_t *)vv)[0];
            break;
        default:
            SET_HDR(th, T_INDIRECT, 0);
            IND_TARGET(th) = v;
        }
    }
    poke(th);
}

/* Force the value on top of the stack, in place. On return the top is a value. */
void force_top(void)
{
    obj_t *p;
    for (;;) {
        p = deref(peek());
        switch (p->tag) {
        case T_INDIRECT:  poke(IND_TARGET(p)); break;
        case T_BLACKHOLE: err_exit(E_INFREC);
        case T_THUNK:     force_thunk(); break;
        default:          return;
        }
    }
}

/* Force the value in stack slot k (1 = the top) in place. */
void force_slot(uint16_t k)
{
    uint16_t at = vsp - k;
    push(vstack[at]);
    force_top();
    vstack[at] = pop();
}

/* Force the top two values in place, then pop both: *b = the top, *a = the one
 * below. A binary operation must force both operands before it reads either. */
void force_two_orig(obj_t **a, obj_t **b, obj_t **ao, obj_t **bo)
{
    force_top();
    force_slot(2);
    *bo = pop(); *b = deref(*bo);
    *ao = pop(); *a = deref(*ao);
}

void force_two(obj_t **a, obj_t **b)
{
    obj_t *ao, *bo;
    force_two_orig(a, b, &ao, &bo);
}
/* force_two's forcing, in its order, but both values STAY on the stack --
 * still roots -- so an operation that allocates in proportion to them can
 * nursery_reserve() before it pops them. The caller drops them (vsp -= 2). */
void view_two(obj_t **a, obj_t **b)
{
    force_top();
    force_slot(2);
    *b = deref(vstack[vsp - 1]);
    *a = deref(vstack[vsp - 2]);
}

/* --- calls ------------------------------------------------------------------------ */
/* f and its argument (on top) -> pc/ctab/env of the callee, both popped. The
 * callee's environment is a two-element LIST: [0] = the closure's captured
 * environment, [1] = the argument, unforced. A set with a __functor attribute
 * is called through it: `s x` == `(s.__functor s) x`. */
void call_enter(void)
{
    obj_t *f, *fo, *x, *e, *fv;
    uint16_t ix;
    for (;;) {
        force_slot(2);                          /* f, forced in place */
        x = pop();
        fo = pop();
        f = deref(fo);
        if (f->tag == T_LAMBDA) break;
        if (f->tag != T_ATTRS) type_err();
        if (!find_attr(fo, str_functor, &ix, &fv)) type_err();
        rpush(x);                               /* the argument, a root across the call */
        push(fv);                               /* the functor, unforced */
        push(fo);                               /* the set, as its own first argument */
        do_call();                              /* g = s.__functor s */
        push(rpop());                           /* then g x */
    }
    stack_check();
    e = alloc(12);
    SET_HDR(e, T_LIST, 2);
    ELEMS(e)[0] = CL_ENV(f);
    ELEMS(e)[1] = x;
    pc = IMAGE_PC(CL_CODE(f));
    ctab = CTAB_AT(CL_CTAB(f));
    env = e;
}

/* The function and its argument are the top two values (argument on top); on
 * return both are replaced by the call's value. Primops use this to apply Nix
 * functions from native code. */
void do_call(void)
{
    const uint8_t *spc = pc;
    obj_t **sctab = ctab;
    rpush(env);
    call_enter();
    run();
    env = rpop();
    ctab = sctab;
    pc = spc;
}

/* A THUNK computing (f x) when forced: its body is apply_code, a chunk the VM
 * carries at APPLY_CODE_OFF in the image bank. */
obj_t *make_apply(obj_t *f, obj_t *x)
{
    obj_t *e = alloc(16), *t;
    SET_HDR(e, T_LIST, 3);
    ELEMS(e)[0] = 0;
    ELEMS(e)[1] = f;
    ELEMS(e)[2] = x;
    t = alloc(12);
    SET_HDR(t, T_THUNK, 0);
    CL_CODE(t) = APPLY_CODE_OFF;
    CL_CTAB(t) = 0;
    CL_ENV(t) = e;
    return t;
}

/* --- closures ---------------------------------------------------------------------- */
/* Both are 12 bytes: header, code and constant-table offsets (the "code
 * pointer"), and the current environment. A lambda is a thunk that takes a
 * parameter: CALL gives it an environment with the argument in slot 1 where
 * FORCE gives a thunk its captured environment directly. */
/* `closed` builds a thunk over the EMPTY environment, which is what an import
 * is: the file is evaluated in its own scope (BYTECODE.md "Imports"). A free
 * variable in an imported file then reaches ENV_GET with nothing to walk and
 * stops with E_BADNBC, rather than silently resolving against the importer. */
static void closure(uint8_t tag, int closed)
{
    uint16_t idx = fetch16();
    obj_t *o;
    if (closed) {
        /* One thunk per imported file, whichever site asks for it, so the file
         * is evaluated once and `import ./f == import ./f` is true. */
        if (idx >= CHUNK_MAX) badnbc();
        if (imports[idx]) { push(imports[idx]); return; }
    }
    o = alloc(12);
    SET_HDR(o, tag, 0);
    CL_CODE(o) = chunk_info[idx].code;
    CL_CTAB(o) = chunk_info[idx].ctab;
    CL_ENV(o) = closed ? 0 : env;
    if (closed) imports[idx] = o;
    push(o);
}

/* --- with ------------------------------------------------------------------------------ */
/* WITH_GET / TRY_WITH_GET: k -> v. Walk the environment chain; at each with
 * environment force its set and look k up. The key and the current environment
 * live on RSTACK across the force. */
static void with_get(int strict)
{
    uint16_t base = rsp, ix;
    obj_t *wv;
    obj_t *cur, *set;
    force_top();
    need(deref(peek()), T_STRING);
    rpush(pop());                               /* the key */
    rpush(env);                                 /* cur */
    for (;;) {
        cur = rstack[base + 1];
        if (cur == 0) break;
        cur = deref(cur);
        if (cur->aux2 & AUX_WITH) {
            obj_t *so;
            push(ELEMS(cur)[1]);
            force_top();                        /* runs bytecode */
            so = pop();
            set = deref(so);
            need(set, T_ATTRS);
            if (find_attr(so, rstack[base], &ix, &wv)) {
                rsp = base;
                push(wv);
                return;
            }
            cur = deref(rstack[base + 1]);
        }
        rstack[base + 1] = ELEMS(cur)[0];       /* cur = parent */
    }
    rsp = base;
    if (strict) err_exit(E_NOVAR);
    push(0);
}

/* --- arithmetic ---------------------------------------------------------------------------- */
/* Both forced, as views: INTs or FLOATs, else E_TYPE. 1 when either is a
 * FLOAT, which makes the operation a float one (docs/FLOAT.md). */
static int binary_nums(obj_t **a, obj_t **b)
{
    force_two(a, b);
    return num_pair(*a, *b);                    /* float.c: the tests are bank 4's */
}

/* The CPU's own 64-bit multiply and divide, for a machine with no card and for
 * checks.verb-economics, which prices ALU64 by masking it off. ORCA/C's own
 * long-long helpers do the work; the sign is applied to unsigned magnitudes
 * rather than trusted to the compiler, because C89 leaves the direction of
 * negative division implementation-defined and CARD-ISA.md requires truncation
 * toward zero. */
static void alu_cpu(uint8_t op, const void *pa, const void *pb, uint8_t *out)
{
    int64_t x, y;
    uint64_t ax, ay, q;
    int neg;
    memcpy(&x, pa, 8);
    memcpy(&y, pb, 8);
    if (op == A_MUL) {
        q = (uint64_t)x * (uint64_t)y;          /* low 64 bits, signed or not */
        memcpy(out, &q, 8);
        return;
    }
    if (y == 0) err_exit(E_DIVZERO);
    neg = (x < 0) != (y < 0);
    ax = (x < 0) ? (uint64_t)0 - (uint64_t)x : (uint64_t)x;
    ay = (y < 0) ? (uint64_t)0 - (uint64_t)y : (uint64_t)y;
    q  = ax / ay;
    if (neg) q = (uint64_t)0 - q;               /* unsigned, so INT64_MIN is fine */
    memcpy(out, &q, 8);
}

/* MUL and DIV go to the card's ALU64 (CARD-ISA.md, the D5 measurement) when
 * there is one and the verb is not masked off; either way the low 64 bits come
 * back, and alu_overflow (prims.c) decides whether they are the answer. */
static void alu(uint8_t op)
{
    obj_t *a, *b, *r;
    uint8_t out[8];
    if (binary_nums(&a, &b)) { float_arith(op == A_MUL ? OP_MUL : OP_DIV, a, b); return; }
    if (slot_x == 0 || verb_off(VERB_NO_ALU64))
        alu_cpu(op, PAYLOAD(a), PAYLOAD(b), out);
    else if (card_alu64(op, PAYLOAD(a), PAYLOAD(b), out))
        err_exit(E_DIVZERO);
    alu_overflow(op, PAYLOAD(a), PAYLOAD(b));
    r = alloc(12);
    SET_HDR(r, T_INT, 0);
    memcpy(PAYLOAD(r), out, 8);
    push(r);
}

static void push_bool(int v) { push(v ? obj_true : obj_false); }

/* --- attribute sets -------------------------------------------------------------------- */
/* a // b: both sorted by name; a merge, b winning ties. Two passes, since the
 * column offsets depend on the count. Nothing here runs bytecode. */
static obj_t *update(obj_t *a, obj_t *b)
{
    uint16_t na = AUX16(a), nb = AUX16(b), ia, ib, k, n, c;
    obj_t **an = ANAMES(a, na), **bn = ANAMES(b, nb);
    obj_t *o;
    for (ia = ib = k = 0; ia < na && ib < nb; ++k) {
        c = str_cmp(deref(an[ia]), deref(bn[ib]));
        if (c == 0) ++ia; else if (c == 2) ++ib; else { ++ia; ++ib; }
    }
    n = k + (na - ia) + (nb - ib);
    o = attrs_alloc(n);
    for (ia = ib = k = 0; k < n; ++k) {
        if (ib == nb) c = 0;
        else if (ia == na) c = 2;
        else c = str_cmp(deref(an[ia]), deref(bn[ib]));
        if (c == 0) {
            HASHES(o)[k] = HASHES(a)[ia];
            ANAMES(o, n)[k] = an[ia];
            AVALUES(o, n)[k] = AVALUES(a, na)[ia];
            ++ia;
        } else {
            HASHES(o)[k] = HASHES(b)[ib];
            ANAMES(o, n)[k] = bn[ib];
            AVALUES(o, n)[k] = AVALUES(b, nb)[ib];
            if (c == 1) ++ia;
            ++ib;
        }
    }
    return o;
}

/* ADD SUB MUL DIV NEG and LT: the operators, shared with the primops of the
 * same names (builtins.add, builtins.lessThan, ...). */
void op_arith(uint16_t op)
{
    obj_t *a, *b;
    switch (op) {
    case OP_ADD:                                /* two INTs add, two STRINGs concatenate */
        view_two(&a, &b);                       /* both still on the stack: see below */
        if (a->tag == T_PATH) {
            /* path + string and path + path: canonPath of the joined text,
             * a PATH (data.c path_join). */
            if (b->tag != T_STRING && b->tag != T_PATH) type_err();
            if (b->tag == T_STRING && STRCTX(b, AUX16(b))) type_err();   /* a store path cannot be appended to a path */
            if ((uint16_t)(AUX16(a) + AUX16(b) + 1) <= AUX16(a)) nomem();
            if (nursery_reserve_n(1, round4(AUX16(a) + AUX16(b) + 1), 4)) view_two(&a, &b);
            vsp -= 2;
            push(path_join(STRBYTES(a), AUX16(a), STRBYTES(b), AUX16(b)));
        } else if (a->tag == T_INT || a->tag == T_FLOAT) {
            vsp -= 2;
            if (num_pair(a, b)) float_arith(OP_ADD, a, b);
            else push(new_int(add_checked(INTVAL(a), INTVAL(b))));
        } else {
            /* String concatenation: the first operand decides what `+` is (a
             * path, an int, else a string), and a string `+` is "${a}${b}" --
             * each side coerced, left first, the contexts united (prims.c). */
            interpolate(2);
        }
        break;
    case OP_SUB:
        if (binary_nums(&a, &b)) float_arith(OP_SUB, a, b);
        else push(new_int(sub_checked(INTVAL(a), INTVAL(b))));
        break;
    case OP_MUL: alu(A_MUL); break;
    case OP_DIV: alu(A_DIV); break;
    case OP_NEG:
        force_top(); a = deref_pop();
        if (a->tag == T_FLOAT) { float_arith(OP_NEG, a, a); break; }
        need(a, T_INT);
        push(new_int(sub_checked(0, INTVAL(a))));   /* -x is 0 - x, as in Nix */
        break;
    case OP_LT: push_bool(cmp_values() == 0); break;
    default: err_exit(E_BADOP);
    }
}

/* --- the dispatch loop --------------------------------------------------------------------- */
void run(void)
{
    uint16_t op, n, i, ix;
    obj_t *a, *b, *o, *ao, *bo, *sv;
    for (;;) {
        /* A boundary: no C local holds an object or a view, so a collection
         * (gc.c lists the other places one runs) and a ring turnover (prims.c
         * also turns it over between strings, holding no view) are safe. */
        if (gc_due())
            gc_collect();
        windows_maybe();
        op = fetch8();
        switch (op) {
        /* --- constants and singletons --- */
        case OP_CONST: push(ctab[fetch16()]); break;
        case OP_POP:   --vsp; break;
        case OP_TRUE:  push(obj_true); break;
        case OP_FALSE: push(obj_false); break;
        case OP_NULL:  push(obj_null); break;

        /* --- arithmetic --- */
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_NEG:
            op_arith(op);
            break;

        /* --- comparison --- */
        case OP_EQ:
            /* Two functions compared at the TOP level are never equal, even the
             * same object -- C++ Nix's rule, which the corpus pins. Inside a list
             * or set the pointer shortcut in values_equal applies. */
            force_two_orig(&a, &b, &ao, &bo);
            if (a->tag == T_LAMBDA && b->tag == T_LAMBDA) { push(obj_false); break; }
            /* Nor is a NaN, even itself: at the top level Nix compares two
             * fresh values, so no pointer shortcut (docs/FLOAT.md). */
            if (a->tag == T_FLOAT || b->tag == T_FLOAT) { push_bool(num_cmp(a, b) == 1); break; }
            push_bool(values_equal(ao, bo));
            break;
        case OP_LT: op_arith(OP_LT); break;
        case OP_GT: push_bool(cmp_values() == 2); break;
        case OP_LE: push_bool(cmp_values() != 2); break;
        case OP_GE: push_bool(cmp_values() != 0); break;
        case OP_NOT:
            force_top(); a = deref_pop(); need(a, T_BOOL);
            push_bool(!BOOLVAL(a));
            break;

        /* --- control flow --- */
        case OP_JMP: n = fetch16(); pc += (int16_t)n; break;
        case OP_JMP_IF_FALSE:                   /* peeks: the value stays for the POP */
            n = fetch16();
            force_top(); a = deref(peek()); need(a, T_BOOL);
            if (!BOOLVAL(a)) pc += (int16_t)n;
            break;
        case OP_JMP_IF_TRUE:
            n = fetch16();
            force_top(); a = deref(peek()); need(a, T_BOOL);
            if (BOOLVAL(a)) pc += (int16_t)n;
            break;
        case OP_ASSERT:
            force_top(); a = deref_pop(); need(a, T_BOOL);
            if (!BOOLVAL(a)) err_exit(E_ASSERT);
            break;

        /* --- lists --- */
        case OP_LIST:                           /* the n values on top, in push order */
            n = fetch16();
            nursery_reserve_n(n, 4, 4);         /* the values are on the stack: roots */
            o = list_alloc(n);
            vsp -= n;
            memcpy(ELEMS(o), &vstack[vsp], 4 * n);
            push(o);
            break;
        case OP_CONCAT:
            view_two(&a, &b);                   /* operands stay rooted for the reserve */
            need(a, T_LIST); need(b, T_LIST);
            n = AUX16(a); i = AUX16(b);
            if ((uint16_t)(n + i) < n) nomem();
            if (nursery_reserve_n(n + i, 4, 4)) view_two(&a, &b);
            vsp -= 2;
            o = list_alloc(n + i);
            memcpy(ELEMS(o), ELEMS(a), 4 * n);
            memcpy(ELEMS(o) + n, ELEMS(b), 4 * i);
            push(o);
            break;

        /* --- attribute sets --- */
        case OP_ATTRS:                          /* n (name, value) pairs, names sorted */
            n = fetch16();
            nursery_reserve_n(n, 12, 4);        /* the pairs are on the stack: roots */
            o = attrs_alloc(n);
            ix = vsp - 2 * n;
            for (i = 0; i < n; ++i)
                attrs_put(o, n, i, vstack[ix + 2 * i], vstack[ix + 2 * i + 1]);
            vsp = ix;
            push(o);
            break;
        case OP_SELECT:
            force_two_orig(&a, &b, &ao, &bo);   /* the set, the key */
            need(a, T_ATTRS); need(b, T_STRING);
            if (!find_attr(ao, bo, &ix, &sv)) err_exit(E_NOATTR);
            push(sv);
            break;
        case OP_TRY_SELECT:                     /* s k -> v | not-found (the null pointer) */
            force_two_orig(&a, &b, &ao, &bo);
            need(b, T_STRING);
            if (a->tag == T_ATTRS && find_attr(ao, bo, &ix, &sv)) push(sv);
            else push(0);
            break;
        case OP_JMP_IF_NOTFOUND:
            n = fetch16();
            if (peek() == 0) pc += (int16_t)n;
            break;
        case OP_HAS_ATTR:
            force_two_orig(&a, &b, &ao, &bo);
            need(b, T_STRING);
            push_bool(a->tag == T_ATTRS && has_attr(ao, bo));
            break;
        case OP_UPDATE:
            view_two(&a, &b);                   /* operands stay rooted for the reserve */
            need(a, T_ATTRS); need(b, T_ATTRS);
            if ((uint16_t)(AUX16(a) + AUX16(b)) < AUX16(a)) nomem();
            if (nursery_reserve_n(AUX16(a) + AUX16(b), 12, 4)) view_two(&a, &b);
            vsp -= 2;
            push(update(a, b));
            break;
        case OP_INTERPOLATE: interpolate(fetch16()); break;

        /* --- environments --- */
        case OP_ENV_NEW:                        /* a LIST of n+1: [0] = the old env, [1..n] = null */
            n = fetch16();
            /* Hot: every `let`. Below 512 bindings the boundary's GC_MARGIN
             * already covers it; env is a root either way. */
            if (n >= GC_MARGIN / 4 - 2) nursery_reserve_n(n + 1, 4, 4);
            o = list_alloc(n + 1);
            ELEMS(o)[0] = env;
            for (i = 1; i <= n; ++i) ELEMS(o)[i] = 0;
            env = o;
            break;
        case OP_ENV_SET:                        /* into a promoted env: the write barrier */
            i = fetch16();
            a = pop();
            store_field(env, 4 + 4 * i, a);
            break;
        case OP_ENV_GET:                        /* depth, i: walk depth parents, read element i */
            n = fetch16();
            i = fetch16();
            o = deref(env);
            while (n--) {
                if (ELEMS(o)[0] == 0) badnbc();
                o = deref(ELEMS(o)[0]);
            }
            a = ELEMS(o)[i];
            if (a == 0) badnbc();               /* a slot read before it was set */
            push(a);
            break;
        case OP_ENV_POP: env = ELEMS(deref(env))[0]; break;

        /* --- closures and calls --- */
        case OP_LAMBDA: closure(T_LAMBDA, 0); break;
        case OP_THUNK:  closure(T_THUNK, 0); break;
        case OP_CLOSED_THUNK: closure(T_THUNK, 1); break;   /* an import */
        case OP_CALL:   do_call(); break;
        case OP_TAIL_CALL: call_enter(); break; /* the callee's RETURN returns to our caller */
        case OP_ATTR_NAME: attr_name(); break;
        case OP_ATTRS_DYN: attrs_dynamic(); break;
        case OP_LAMBDA_ARGS: lambda_args(); break;
        case OP_WITH_PUSH:                      /* the set (unforced) becomes a marked environment */
            a = pop();
            o = alloc(12);
            SET_HDR(o, T_LIST, 2);
            o->aux2 = AUX_WITH;
            ELEMS(o)[0] = env;
            ELEMS(o)[1] = a;
            env = o;
            break;
        case OP_WITH_GET:     with_get(1); break;
        case OP_TRY_WITH_GET: with_get(0); break;

        /* --- thunks --- */
        case OP_FORCE:  force_top(); break;
        case OP_RETURN: return;
        case OP_PRIM:   op_prim(); break;

        default:
            err_exit(E_BADOP);
        }
    }
}
