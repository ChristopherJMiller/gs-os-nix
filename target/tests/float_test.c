/* float_test.c -- target/vm/c/float.c, built by ORCA/C, against the host's
 * answers (scripts/float-vectors.c writes them, checks.float-vectors runs
 * this): every vector's operation recomputed here and compared bit for bit,
 * every %f string byte for byte. At $0300 the number of wrong answers (u16),
 * at $0302 the first wrong one's index ($8000 + i for a format; $FFFF for
 * none), status $01 at $0320 -- what a2min --run-bin prints.
 *
 * The entry is the first function of the first object, as smoke.c's is. */
#pragma noroot
#pragma optimize 15

#include "vm.h"

extern const uint16_t n_arith, n_fmt;
extern const uint8_t arith[][25];
extern const uint8_t fmt_bits[][8];
extern const char *const fmt_want[];

static uint64_t get(const uint8_t *p);

void entry(void)
{
    uint16_t i, bad = 0, first = 0xFFFF, len;
    uint64_t a, b, r, w;
    int64_t v;
    int ok;
    const uint8_t *t;
    char buf[F_FORMAT_MAX];
    for (i = 0; i < n_arith; ++i) {
        t = arith[i];
        a = get(t + 1);
        b = get(t + 9);
        w = get(t + 17);
        switch (t[0]) {
        case 0: r = f_add(a, b); break;
        case 1: r = f_sub(a, b); break;
        case 2: r = f_mul(a, b); break;
        case 3: r = f_div(a, b); break;
        case 4: r = (uint64_t)f_eq(a, b); break;
        case 5: r = (uint64_t)f_lt(a, b); break;
        case 6: r = f_from_int((int64_t)a); break;
        default:                                /* floor, ceil: b says whether in range */
            ok = f_round_int(a, t[0] == 8, &v);
            r = ok ? (uint64_t)v : 0;
            if ((uint64_t)ok != b) r = ~w;
        }
        if (r != w) {
            ++bad;
            if (first == 0xFFFF) first = i;
        }
    }
    for (i = 0; i < n_fmt; ++i) {
        len = f_format(get(fmt_bits[i]), buf);
        if (len != strlen(fmt_want[i]) || memcmp(buf, fmt_want[i], len) != 0) {
            ++bad;
            if (first == 0xFFFF) first = 0x8000 | i;
        }
    }
    *(volatile uint16_t *)0x0300 = bad;
    *(volatile uint16_t *)0x0302 = first;
    *(volatile uint8_t *)0x0320 = 1;
}

static uint64_t get(const uint8_t *p)
{
    uint64_t u;
    memcpy(&u, p, 8);
    return u;
}
