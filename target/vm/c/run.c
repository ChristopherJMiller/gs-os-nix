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
/* The thunk on top of the stack: blackhole it, remember it on FSTACK (a caught
 * throw un-blackholes it), run its body with its captured environment, then
 * update it in place with the value -- a value of 12 bytes or fewer overwrites
 * the thunk; anything larger makes the thunk an INDIRECT to it. The pointer on
 * top is the thunk's afterwards (it may have moved during the body). */
static void force_thunk(void)
{
    obj_t *th = peek(), *v;
    const uint8_t *spc = pc;
    obj_t **sctab = ctab;
    stack_check();
    th->tag = T_BLACKHOLE;
    if (fstack_n >= FSTACK_N) nomem();
    fstack[fstack_n++] = th;
    rpush(env);
    pc = IMAGE_PC(CL_CODE(th));
    ctab = CTAB_AT(CL_CTAB(th));
    env = CL_ENV(th);
    run();                                      /* leaves the value above the thunk */
    env = rpop();
    ctab = sctab;
    pc = spc;
    th = fstack[--fstack_n];
    v = pop();
    if (!IS_RAM(v)) type_err();                 /* a mature result: not yet */
    th = deref(th);
    switch (v->tag) {
    case T_INT: case T_LAMBDA: case T_THUNK:
        ((uint32_t *)th)[2] = ((uint32_t *)v)[2];
        /* fall through */
    case T_BOOL: case T_NULL:
        ((uint32_t *)th)[1] = ((uint32_t *)v)[1];
        ((uint32_t *)th)[0] = ((uint32_t *)v)[0];
        break;
    default:
        SET_HDR(th, T_INDIRECT, 0);
        IND_TARGET(th) = v;
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
void force_two(obj_t **a, obj_t **b)
{
    force_top();
    force_slot(2);
    *b = deref_pop();
    *a = deref_pop();
}

/* --- calls ------------------------------------------------------------------------ */
/* f and its argument (on top) -> pc/ctab/env of the callee, both popped. The
 * callee's environment is a two-element LIST: [0] = the closure's captured
 * environment, [1] = the argument, unforced. A set with a __functor attribute
 * is called through it: `s x` == `(s.__functor s) x`. */
void call_enter(void)
{
    obj_t *f, *x, *e;
    uint16_t ix;
    for (;;) {
        force_slot(2);                          /* f, forced in place */
        x = pop();
        f = deref_pop();
        if (f->tag == T_LAMBDA) break;
        if (f->tag != T_ATTRS) type_err();
        if (!find_attr(f, str_functor, &ix)) type_err();
        rpush(x);                               /* the argument, a root across the call */
        push(AVALUES(f, AUX16(f))[ix]);         /* the functor, unforced */
        push(f);                                /* the set, as its own first argument */
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
static void closure(uint8_t tag)
{
    uint16_t idx = fetch16();
    obj_t *o = alloc(12);
    SET_HDR(o, tag, 0);
    CL_CODE(o) = chunk_info[idx].code;
    CL_CTAB(o) = chunk_info[idx].ctab;
    CL_ENV(o) = env;
    push(o);
}

/* --- with ------------------------------------------------------------------------------ */
/* WITH_GET / TRY_WITH_GET: k -> v. Walk the environment chain; at each with
 * environment force its set and look k up. The key and the current environment
 * live on RSTACK across the force. */
static void with_get(int strict)
{
    uint16_t base = rsp, ix;
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
            push(ELEMS(cur)[1]);
            force_top();                        /* runs bytecode */
            set = deref_pop();
            need(set, T_ATTRS);
            if (find_attr(set, deref(rstack[base]), &ix)) {
                obj_t *v = AVALUES(set, AUX16(set))[ix];
                rsp = base;
                push(v);
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
static void binary_ints(obj_t **a, obj_t **b)
{
    force_two(a, b);
    need(*a, T_INT);
    need(*b, T_INT);
}

/* MUL and DIV go to the card's ALU64 (CARD-ISA.md, the D5 measurement). */
static void alu(uint8_t op)
{
    obj_t *a, *b, *r;
    uint8_t out[8];
    binary_ints(&a, &b);
    if (slot_x == 0) err_exit(E_NOCARD);
    if (card_alu64(op, PAYLOAD(a), PAYLOAD(b), out)) err_exit(E_DIVZERO);
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

/* INTERPOLATE n: the n parts on the stack, forced, must be STRINGs; concatenate. */
static void interpolate(uint16_t n)
{
    uint16_t i, len = 0, at;
    obj_t *o, *s;
    for (i = 0; i < n; ++i) force_slot(n - i);
    at = vsp - n;
    for (i = 0; i < n; ++i) {
        s = deref(vstack[at + i]);
        need(s, T_STRING);
        if ((uint16_t)(len + AUX16(s)) < len) nomem();
        len += AUX16(s);
    }
    o = str_alloc(len);
    len = 0;
    for (i = 0; i < n; ++i) {
        s = deref(vstack[at + i]);
        memcpy(STRBYTES(o) + len, STRBYTES(s), AUX16(s));
        len += AUX16(s);
    }
    vsp = at;
    push(o);
}

/* ADD SUB MUL DIV NEG and LT: the operators, shared with the primops of the
 * same names (builtins.add, builtins.lessThan, ...). */
void op_arith(uint16_t op)
{
    obj_t *a, *b;
    switch (op) {
    case OP_ADD:                                /* two INTs add, two STRINGs concatenate */
        force_two(&a, &b);
        if (a->tag == T_STRING) {
            need(b, T_STRING);
            push(str_concat(a, b));
        } else {
            need(a, T_INT); need(b, T_INT);
            push(new_int(INTVAL(a) + INTVAL(b)));
        }
        break;
    case OP_SUB: binary_ints(&a, &b); push(new_int(INTVAL(a) - INTVAL(b))); break;
    case OP_MUL: alu(A_MUL); break;
    case OP_DIV: alu(A_DIV); break;
    case OP_NEG:
        force_top(); a = deref_pop(); need(a, T_INT);
        push(new_int(-INTVAL(a)));
        break;
    case OP_LT: push_bool(cmp_values() == 0); break;
    default: err_exit(E_BADOP);
    }
}

/* --- the dispatch loop --------------------------------------------------------------------- */
void run(void)
{
    uint16_t op, n, i, ix;
    obj_t *a, *b, *o;
    for (;;) {
        /* the only place a collection can happen (gc_maybe, inlined) */
        if (*(volatile uint8_t *)GC_STRESS_ADDR ? nfree != gc_water : nfree >= NURSERY_END - GC_MARGIN)
            gc_collect();
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
            force_two(&a, &b);
            if (a->tag == T_LAMBDA && b->tag == T_LAMBDA) { push(obj_false); break; }
            push_bool(values_equal(a, b));
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
            o = list_alloc(n);
            vsp -= n;
            memcpy(ELEMS(o), &vstack[vsp], 4 * n);
            push(o);
            break;
        case OP_CONCAT:
            force_two(&a, &b);
            need(a, T_LIST); need(b, T_LIST);
            n = AUX16(a); i = AUX16(b);
            if ((uint16_t)(n + i) < n) nomem();
            o = list_alloc(n + i);
            memcpy(ELEMS(o), ELEMS(a), 4 * n);
            memcpy(ELEMS(o) + n, ELEMS(b), 4 * i);
            push(o);
            break;

        /* --- attribute sets --- */
        case OP_ATTRS:                          /* n (name, value) pairs, names sorted */
            n = fetch16();
            o = attrs_alloc(n);
            ix = vsp - 2 * n;
            for (i = 0; i < n; ++i)
                attrs_put(o, n, i, vstack[ix + 2 * i], vstack[ix + 2 * i + 1]);
            vsp = ix;
            push(o);
            break;
        case OP_SELECT:
            force_two(&a, &b);                  /* the set, the key */
            need(a, T_ATTRS); need(b, T_STRING);
            if (!find_attr(a, b, &ix)) err_exit(E_NOATTR);
            push(AVALUES(a, AUX16(a))[ix]);
            break;
        case OP_TRY_SELECT:                     /* s k -> v | not-found (the null pointer) */
            force_two(&a, &b);
            need(b, T_STRING);
            if (a->tag == T_ATTRS && find_attr(a, b, &ix)) push(AVALUES(a, AUX16(a))[ix]);
            else push(0);
            break;
        case OP_JMP_IF_NOTFOUND:
            n = fetch16();
            if (peek() == 0) pc += (int16_t)n;
            break;
        case OP_HAS_ATTR:
            force_two(&a, &b);
            need(b, T_STRING);
            push_bool(a->tag == T_ATTRS && find_attr(a, b, &ix));
            break;
        case OP_UPDATE:
            force_two(&a, &b);
            need(a, T_ATTRS); need(b, T_ATTRS);
            push(update(a, b));
            break;
        case OP_INTERPOLATE: interpolate(fetch16()); break;

        /* --- environments --- */
        case OP_ENV_NEW:                        /* a LIST of n+1: [0] = the old env, [1..n] = null */
            n = fetch16();
            o = list_alloc(n + 1);
            ELEMS(o)[0] = env;
            for (i = 1; i <= n; ++i) ELEMS(o)[i] = 0;
            env = o;
            break;
        case OP_ENV_SET:
            i = fetch16();
            a = pop();
            ELEMS(deref(env))[i] = a;
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
        case OP_LAMBDA: closure(T_LAMBDA); break;
        case OP_THUNK:  closure(T_THUNK); break;
        case OP_CALL:   do_call(); break;
        case OP_TAIL_CALL: call_enter(); break; /* the callee's RETURN returns to our caller */
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
