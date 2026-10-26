/* numbers.c -- the VM's floats as values (docs/FLOAT.md): FLOAT objects,
 * arithmetic and comparison when either operand is one, floor and ceil, and
 * toString's %f. float.c is the arithmetic itself, on bit patterns, with no
 * VM in it -- checks.float-vectors builds and tests it alone.
 */
#pragma noroot
#pragma optimize 15

#include "vm.h"

/* With float.c, a segment of its own above bank 0 (VM.md). */
segment "float";

obj_t *new_float(uint64_t bits)
{
    obj_t *o = alloc(12);
    SET_HDR(o, T_FLOAT, 0);
    memcpy(PAYLOAD(o), &bits, 8);
    return o;
}

/* A number (a view) as a float: a FLOAT's bits, or an INT rounded to nearest
 * as C++ converts it. E_TYPE for anything else. */
static uint64_t num_bits(obj_t *v)
{
    uint64_t u;
    if (v->tag == T_FLOAT) {
        memcpy(&u, PAYLOAD(v), 8);
        return u;
    }
    if (v->tag != T_INT) type_err();
    return f_from_int(INTVAL(v));
}

/* op on the forced views a and b -- one of them a FLOAT, so both become
 * floats; b is not read for OP_NEG -- with the result, a FLOAT, pushed.
 * Division by zero is E_DIVZERO for floats too, as Nix's prim_div tests the
 * divisor first; -x is 0 - x, so -(0.0) is +0. */
void float_arith(uint16_t op, obj_t *a, obj_t *b)
{
    uint64_t x = num_bits(a), y = 0, r;
    if (op != OP_NEG) y = num_bits(b);          /* not a ?: (TOOLCHAIN.md "Known miscompiles") */
    switch (op) {
    case OP_ADD: r = f_add(x, y); break;
    case OP_SUB: r = f_sub(x, y); break;
    case OP_MUL: r = f_mul(x, y); break;
    case OP_DIV:
        if (f_eq(y, 0)) err_exit(E_DIVZERO);
        r = f_div(x, y);
        break;
    default:     r = f_sub(0, x);
    }
    push(new_float(r));
}

/* The views a and b, at least one a FLOAT, compared as doubles: 0 a < b, 1
 * equal, 2 a > b, 3 unordered (a NaN); -1 if either is not a number. */
int num_cmp(obj_t *a, obj_t *b)
{
    uint64_t x, y;
    if ((a->tag != T_INT && a->tag != T_FLOAT) || (b->tag != T_INT && b->tag != T_FLOAT)) return -1;
    x = num_bits(a);
    y = num_bits(b);
    if (f_lt(x, y)) return 0;
    if (f_eq(x, y)) return 1;
    if (f_lt(y, x)) return 2;
    return 3;
}

int num_order(obj_t *a, obj_t *b)
{
    int c = num_cmp(a, b);
    if (c < 0) type_err();
    return c;
}

int num_pair(obj_t *a, obj_t *b)
{
    if ((a->tag != T_INT && a->tag != T_FLOAT) || (b->tag != T_INT && b->tag != T_FLOAT)) type_err();
    return a->tag == T_FLOAT || b->tag == T_FLOAT;
}

/* toString of a FLOAT (a view): %f, a fresh STRING. The digits are made first,
 * into the C stack, so the reserve may collect. */
obj_t *float_str(obj_t *v)
{
    char buf[F_FORMAT_MAX];
    uint16_t n = f_format(num_bits(v), buf);
    obj_t *r;
    nursery_reserve(round4(n) + 8);
    r = str_alloc(n);
    memcpy(STRBYTES(r), buf, n);
    return r;
}

/* floor and ceil: a FLOAT, or an INT, to an INT in [-2^63, 2^63) (E_TYPE
 * otherwise, Nix's "not in the range of NixInt"). An INT comes back unchanged
 * or is an error: Nix's own guard against the precision a double loses
 * (issue 12899). */
static void round_prim(int up)
{
    obj_t *v;
    int64_t r;
    (void)force_pop_view(&v);
    if (!f_round_int(num_bits(v), up, &r)) type_err();
    if (v->tag == T_INT && r != INTVAL(v)) type_err();
    push(new_int(r));
}

void prim_floor(void) { round_prim(0); }
void prim_ceil(void)  { round_prim(1); }
