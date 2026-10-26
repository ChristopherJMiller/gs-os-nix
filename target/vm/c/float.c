/* float.c -- IEEE-754 binary64 in software (docs/FLOAT.md).
 *
 * A float is its 64-bit pattern. Each operation rounds to nearest, ties to
 * even, as the host's hardware does, with subnormals, infinities and x86's NaN
 * behaviour (the default NaN is 0xFFF8000000000000; a NaN operand comes out
 * quieted). The structure follows Berkeley SoftFloat 3 (BSD, John Hauser):
 * a significand with its leading bit at bit 62 and ten round bits below, and
 * an exponent one less than the biased one, so packing ADDS the leading bit
 * into the exponent field. Division is a long division, bit by bit, which is
 * slow and exact. toString's %f is exact too: the value's integer M * 2^E,
 * times 10^6, rounded, in a bignum of 16-bit limbs.
 *
 * ORCA/C's int is 16 bits, so everything here is an explicit width and nothing
 * leans on an integer promotion. checks.float-vectors runs this file, built
 * by ORCA/C, against answers the host's own arithmetic and printf gave.
 */
#pragma noroot
#pragma optimize 15

#include "vm.h"

/* A segment of its own: "prims" fills bank 4 (VM.md). */
segment "float";

#define SIGN_BIT  ((uint64_t)1 << 63)
#define FRAC_MASK (((uint64_t)1 << 52) - 1)
#define F_NAN     ((uint64_t)0xFFF8000000000000)    /* x86's default NaN */
#define QUIET     ((uint64_t)1 << 51)

#define F_EXP(a)  ((int16_t)((uint16_t)((a) >> 52) & 0x7FF))
#define F_FRAC(a) ((a) & FRAC_MASK)
#define F_SIGN(a) ((uint16_t)((a) >> 63))
#define PACK(s, e, sig) (((uint64_t)(s) << 63) + ((uint64_t)(uint16_t)(e) << 52) + (sig))
#define IS_NAN(a) (F_EXP(a) == 0x7FF && F_FRAC(a) != 0)

/* a << n and a >> n for a variable n, through 32-bit halves: ORCA/C would
 * otherwise link ~SHL8 and ~LSHR8 into bank 0 (scripts/bank-gate.py), and the
 * 32-bit ones are there already. */
static uint64_t shl(uint64_t a, uint16_t n)
{
    uint32_t hi = (uint32_t)(a >> 32), lo = (uint32_t)a;
    if (n >= 64) return 0;
    if (n >= 32) { hi = lo << (n - 32); lo = 0; }
    else if (n) { hi = (hi << n) | (lo >> (32 - n)); lo <<= n; }
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t shr(uint64_t a, uint16_t n)
{
    uint32_t hi = (uint32_t)(a >> 32), lo = (uint32_t)a;
    if (n >= 64) return 0;
    if (n >= 32) { lo = hi >> (n - 32); hi = 0; }
    else if (n) { lo = (lo >> n) | (hi << (32 - n)); hi >>= n; }
    return ((uint64_t)hi << 32) | lo;
}

static uint16_t clz64(uint64_t a)
{
    uint16_t n = 0;
    if (!a) return 64;
    if (!(a >> 32)) { n += 32; a <<= 32; }
    if (!(a >> 48)) { n += 16; a <<= 16; }
    if (!(a >> 56)) { n += 8; a <<= 8; }
    if (!(a >> 60)) { n += 4; a <<= 4; }
    if (!(a >> 62)) { n += 2; a <<= 2; }
    if (!(a >> 63)) n += 1;
    return n;
}

/* a >> dist, with any bit shifted out ORed into bit 0. */
static uint64_t shr_jam(uint64_t a, uint16_t dist)
{
    if (!dist) return a;
    if (dist >= 63) return a != 0;
    return shr(a, dist) | (shl(a, 64 - dist) != 0);
}

static uint64_t propagate_nan(uint64_t a, uint64_t b)
{
    return IS_NAN(a) ? a | QUIET : b | QUIET;
}

/* Round and pack: sig's leading bit at 62 (or below, for a subnormal), exp one
 * less than the biased exponent. Ties to even. */
static uint64_t round_pack(uint16_t sign, int16_t exp, uint64_t sig)
{
    uint16_t round_bits = (uint16_t)sig & 0x3FF;
    if ((uint16_t)exp >= 0x7FD) {
        if (exp < 0) {
            sig = shr_jam(sig, (uint16_t)-exp);
            exp = 0;
            round_bits = (uint16_t)sig & 0x3FF;
        } else if (exp > 0x7FD || sig + 0x200 >= SIGN_BIT) {
            return PACK(sign, 0x7FF, 0);        /* overflow: infinity */
        }
    }
    sig = (sig + 0x200) >> 10;
    if (round_bits == 0x200) sig &= ~(uint64_t)1;  /* a tie: to even */
    if (!sig) exp = 0;
    return PACK(sign, exp, sig);
}

/* Normalise sig (leading bit anywhere) and round. */
static uint64_t norm_round_pack(uint16_t sign, int16_t exp, uint64_t sig)
{
    uint16_t dist = clz64(sig) - 1;
    exp -= (int16_t)dist;
    if (dist >= 10 && (uint16_t)exp < 0x7FD)
        return PACK(sign, sig ? exp : 0, shl(sig, dist - 10));
    return round_pack(sign, exp, shl(sig, dist));
}

static uint64_t add_mags(uint64_t a, uint64_t b, uint16_t sign)
{
    int16_t ea = F_EXP(a), eb = F_EXP(b), d = ea - eb, ez;
    uint64_t sa = F_FRAC(a), sb = F_FRAC(b), sz;
    if (!d) {
        if (!ea) return a + sb;                 /* two subnormals: exact */
        if (ea == 0x7FF) { if (sa | sb) return propagate_nan(a, b); return a; }
        ez = ea;
        sz = (((uint64_t)1 << 53) + sa + sb) << 9;
    } else {
        sa <<= 9;
        sb <<= 9;
        if (d < 0) {
            if (eb == 0x7FF) { if (sb) return propagate_nan(a, b); return PACK(sign, 0x7FF, 0); }
            ez = eb;
            sa = ea ? sa + ((uint64_t)1 << 61) : sa << 1;
            sa = shr_jam(sa, (uint16_t)-d);
        } else {
            if (ea == 0x7FF) { if (sa) return propagate_nan(a, b); return a; }
            ez = ea;
            sb = eb ? sb + ((uint64_t)1 << 61) : sb << 1;
            sb = shr_jam(sb, (uint16_t)d);
        }
        sz = ((uint64_t)1 << 61) + sa + sb;
        if (sz < ((uint64_t)1 << 62)) { --ez; sz <<= 1; }
    }
    return round_pack(sign, ez, sz);
}

static uint64_t sub_mags(uint64_t a, uint64_t b, uint16_t sign)
{
    int16_t ea = F_EXP(a), eb = F_EXP(b), d = ea - eb, ez;
    uint64_t sa = F_FRAC(a), sb = F_FRAC(b), sz;
    uint16_t dist;
    if (!d) {
        if (ea == 0x7FF) { if (sa | sb) return propagate_nan(a, b); return F_NAN; }   /* inf - inf */
        if (sa == sb) return 0;                 /* x - x is +0, rounding to nearest */
        if (ea) --ea;
        if (sa < sb) { sign ^= 1; sz = sb - sa; } else { sz = sa - sb; }
        dist = clz64(sz) - 11;
        ez = ea - (int16_t)dist;
        if (ez < 0) { dist = (uint16_t)ea; ez = 0; }
        return PACK(sign, ez, shl(sz, dist));
    }
    sa <<= 10;
    sb <<= 10;
    if (d < 0) {
        sign ^= 1;
        if (eb == 0x7FF) { if (sb) return propagate_nan(a, b); return PACK(sign, 0x7FF, 0); }
        sa += ea ? ((uint64_t)1 << 62) : sa;
        sa = shr_jam(sa, (uint16_t)-d);
        sb |= (uint64_t)1 << 62;
        ez = eb;
        sz = sb - sa;
    } else {
        if (ea == 0x7FF) { if (sa) return propagate_nan(a, b); return a; }
        sb += eb ? ((uint64_t)1 << 62) : sb;
        sb = shr_jam(sb, (uint16_t)d);
        sa |= (uint64_t)1 << 62;
        ez = ea;
        sz = sa - sb;
    }
    return norm_round_pack(sign, ez - 1, sz);
}

uint64_t f_add(uint64_t a, uint64_t b)
{
    if (F_SIGN(a) == F_SIGN(b)) return add_mags(a, b, F_SIGN(a)); return sub_mags(a, b, F_SIGN(a));
}

uint64_t f_sub(uint64_t a, uint64_t b)
{
    if (F_SIGN(a) == F_SIGN(b)) return sub_mags(a, b, F_SIGN(a)); return add_mags(a, b, F_SIGN(a));
}

/* A subnormal's significand normalised to bit 52; *exp gets its exponent. */
static uint64_t norm_sub(uint64_t sig, int16_t *exp)
{
    uint16_t dist = clz64(sig) - 11;
    *exp = 1 - (int16_t)dist;
    return shl(sig, dist);
}

uint64_t f_mul(uint64_t a, uint64_t b)
{
    int16_t ea = F_EXP(a), eb = F_EXP(b), ez;
    uint64_t sa = F_FRAC(a), sb = F_FRAC(b), sz, hi, lo, mid, mid1;
    uint32_t a1, a0, b1, b0;
    uint16_t sign = F_SIGN(a) ^ F_SIGN(b);
    if (ea == 0x7FF) {
        if (sa || (eb == 0x7FF && sb)) return propagate_nan(a, b);
        return (eb | sb) ? PACK(sign, 0x7FF, 0) : F_NAN;       /* inf * 0 */
    }
    if (eb == 0x7FF) {
        if (sb) return propagate_nan(a, b);
        return (ea | sa) ? PACK(sign, 0x7FF, 0) : F_NAN;
    }
    if (!ea) {
        if (!sa) return PACK(sign, 0, 0);
        sa = norm_sub(sa, &ea);
    }
    if (!eb) {
        if (!sb) return PACK(sign, 0, 0);
        sb = norm_sub(sb, &eb);
    }
    ez = ea + eb - 0x3FF;
    sa = (sa | ((uint64_t)1 << 52)) << 10;
    sb = (sb | ((uint64_t)1 << 52)) << 11;
    /* the 128-bit product, from 32-bit halves */
    a1 = (uint32_t)(sa >> 32); a0 = (uint32_t)sa;
    b1 = (uint32_t)(sb >> 32); b0 = (uint32_t)sb;
    lo = (uint64_t)a0 * b0;
    mid1 = (uint64_t)a1 * b0;
    mid = mid1 + (uint64_t)a0 * b1;
    hi = (uint64_t)a1 * b1;
    hi += ((uint64_t)(mid < mid1) << 32) | (mid >> 32);
    mid <<= 32;
    lo += mid;
    hi += lo < mid;
    sz = hi | (lo != 0);
    if (sz < ((uint64_t)1 << 62)) { --ez; sz <<= 1; }
    return round_pack(sign, ez, sz);
}

/* b is not zero: Nix's / refuses that before it gets here. */
uint64_t f_div(uint64_t a, uint64_t b)
{
    int16_t ea = F_EXP(a), eb = F_EXP(b), ez, i;
    uint64_t sa = F_FRAC(a), sb = F_FRAC(b), q, rem;
    uint16_t sign = F_SIGN(a) ^ F_SIGN(b);
    if (ea == 0x7FF) {
        if (sa) return propagate_nan(a, b);
        if (eb == 0x7FF) { if (sb) return propagate_nan(a, b); return F_NAN; }   /* inf / inf */
        return PACK(sign, 0x7FF, 0);
    }
    if (eb == 0x7FF) { if (sb) return propagate_nan(a, b); return PACK(sign, 0, 0); }
    if (!eb) {
        if (!sb) return (ea | sa) ? PACK(sign, 0x7FF, 0) : F_NAN;
        sb = norm_sub(sb, &eb);
    }
    if (!ea) {
        if (!sa) return PACK(sign, 0, 0);
        sa = norm_sub(sa, &ea);
    }
    sa |= (uint64_t)1 << 52;
    sb |= (uint64_t)1 << 52;
    /* q = floor(sa * 2^63 / sb), in (2^62, 2^64): one bit at a time */
    rem = sa;
    q = 0;
    if (rem >= sb) { q = 1; rem -= sb; }
    for (i = 0; i < 63; ++i) {
        rem <<= 1;
        q <<= 1;
        if (rem >= sb) { rem -= sb; q |= 1; }
    }
    if (rem) q |= 1;                            /* sticky */
    ez = ea - eb + 0x3FD;
    if (q >= SIGN_BIT) { q = (q >> 1) | (q & 1); ++ez; }
    return round_pack(sign, ez, q);
}

uint64_t f_from_int(int64_t v)
{
    uint16_t sign = v < 0;
    uint64_t m = sign ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    if (!(m & ~SIGN_BIT)) return sign ? PACK(1, 0x43E, 0) : 0;      /* 0, or the minimum */
    return norm_round_pack(sign, 0x43C, m);
}

int f_eq(uint64_t a, uint64_t b)
{
    if (IS_NAN(a) || IS_NAN(b)) return 0;
    return a == b || !((a | b) & ~SIGN_BIT);   /* -0 == +0 */
}

int f_lt(uint64_t a, uint64_t b)
{
    uint16_t sa = F_SIGN(a), sb = F_SIGN(b);
    if (IS_NAN(a) || IS_NAN(b)) return 0;
    if (sa != sb) return sa && ((a | b) & ~SIGN_BIT) != 0;
    return a != b && (sa ^ (a < b));
}

/* floor (up = 0) or ceil (up = 1) of a, as an int64, into *out: 0 if the
 * result is outside [-2^63, 2^63), or a is a NaN (Nix: "not in the range"). */
int f_round_int(uint64_t a, int up, int64_t *out)
{
    int16_t e = F_EXP(a), sh;
    uint16_t neg = F_SIGN(a);
    uint64_t sig = F_FRAC(a), m, lost;
    if (e == 0x7FF) return 0;
    if (e < 0x3FF) {                            /* |a| < 1 */
        m = (e | sig) && (neg ? !up : up);      /* 1 away from zero, or 0 */
    } else if (e >= 0x3FF + 63) {
        if (neg && e == 0x3FF + 63 && !sig) { *out = (int64_t)SIGN_BIT; return 1; }   /* -2^63 */
        return 0;
    } else {
        sig |= (uint64_t)1 << 52;
        sh = e - 0x433;                         /* e - 1023 - 52 */
        if (sh >= 0) {
            m = shl(sig, (uint16_t)sh);
        } else {
            m = shr(sig, (uint16_t)-sh);
            lost = sig & (shl(1, (uint16_t)-sh) - 1);
            if (lost && (neg ? !up : up)) ++m;  /* away from zero */
        }
    }
    /* Not neg ? (int64_t)(0 - m) : (int64_t)m: ORCA/C miscompiles a cast of
     * an unsigned 64-bit difference inside ?: (TOOLCHAIN.md, c-lint). */
    if (neg) m = (uint64_t)0 - m;
    *out = (int64_t)m;
    return 1;
}

/* --- toString: %f, exactly ---------------------------------------------------------------- */
#define NL 72           /* 16-bit limbs: 1152 bits; 2^1024 * 10^6 needs 1044 */

/* x * 1000, as (x << 10) - (x << 4) - (x << 3): a 32-bit multiply would link
 * ~UMUL4 into bank 0. */
static uint16_t bn_mul1000(uint16_t *x, uint16_t n)
{
    uint16_t i;
    uint32_t c = 0, v;
    for (i = 0; i < n; ++i) {
        v = x[i];
        c += (v << 10) - (v << 4) - (v << 3);
        x[i] = (uint16_t)c;
        c >>= 16;
    }
    if (c) x[n++] = (uint16_t)c;
    return n;
}

static uint16_t bn_shl(uint16_t *x, uint16_t n, uint16_t bits)
{
    uint16_t w = bits >> 4, b = bits & 15, i;
    if (w) {
        for (i = n; i-- > 0; ) x[i + w] = x[i];
        for (i = 0; i < w; ++i) x[i] = 0;
        n += w;
    }
    if (b) {
        x[n] = 0;
        for (i = n; i > w; --i) x[i] = (uint16_t)((((uint32_t)x[i] << 16 | x[i - 1]) << b) >> 16);
        x[w] = (uint16_t)((uint32_t)x[w] << b);
        ++n;
    }
    while (n && !x[n - 1]) --n;
    return n;
}

static int bn_bit(const uint16_t *x, uint16_t n, uint16_t pos)
{
    return (pos >> 4) < n && (((uint32_t)x[pos >> 4] >> (pos & 15)) & 1);
}

static int bn_any_below(const uint16_t *x, uint16_t n, uint16_t pos)
{
    uint16_t w = pos >> 4, i;
    for (i = 0; i < w && i < n; ++i) if (x[i]) return 1;
    return w < n && ((uint32_t)x[w] & (((uint32_t)1 << (pos & 15)) - 1)) != 0;
}

/* x >> s, rounded to nearest, ties to even. */
static uint16_t bn_shr_round(uint16_t *x, uint16_t n, uint16_t s)
{
    uint16_t w = s >> 4, b = s & 15, i, m;
    int half = s && bn_bit(x, n, s - 1), sticky = s > 1 && bn_any_below(x, n, s - 1);
    if (w >= n) {
        m = 0;
    } else {
        m = n - w;
        for (i = 0; i < m; ++i) {
            x[i] = (uint16_t)(((uint32_t)(i + w + 1 < n ? x[i + w + 1] : 0) << 16 | x[i + w]) >> b);
        }
    }
    while (m && !x[m - 1]) --m;
    if (half && (sticky || (m && (x[0] & 1)))) {        /* round up */
        for (i = 0; i < m && !++x[i]; ++i) ;
        if (i == m) x[m++] = 1;
    }
    return m;
}

static uint16_t bn_div_small(uint16_t *x, uint16_t n, uint16_t d, uint16_t *rem)
{
    uint16_t i;
    uint32_t r = 0;
    for (i = n; i-- > 0; ) {
        r = (r << 16) | x[i];
        x[i] = (uint16_t)(r / d);
        r %= d;
    }
    *rem = (uint16_t)r;
    while (n && !x[n - 1]) --n;
    return n;
}

static uint16_t put_lit(char *buf, uint16_t at, const char *s, uint16_t n)
{
    memcpy(buf + at, s, n);
    return at + n;
}

/* a as printf("%f") prints it, into buf (F_FORMAT_MAX bytes): its length. */
uint16_t f_format(uint64_t a, char *buf)
{
    uint16_t x[NL], n, i, r, len = 0, nd;
    int16_t e = F_EXP(a), E;
    uint64_t m = F_FRAC(a);
    char d[F_FORMAT_MAX];
    if (F_SIGN(a)) buf[len++] = '-';
    if (e == 0x7FF) return put_lit(buf, len, m ? "nan" : "inf", 3);
    if (e) { m |= (uint64_t)1 << 52; E = e - 0x433; } else { E = -0x432; }
    x[0] = (uint16_t)m; x[1] = (uint16_t)(m >> 16); x[2] = (uint16_t)(m >> 32); x[3] = (uint16_t)(m >> 48);
    n = 4;
    while (n && !x[n - 1]) --n;
    n = bn_mul1000(x, n);                       /* * 10^6 */
    n = bn_mul1000(x, n);
    if (E >= 0) n = bn_shl(x, n, (uint16_t)E);
    else n = bn_shr_round(x, n, (uint16_t)-E);
    nd = 0;                                     /* the digits, least significant first */
    while (n) {
        n = bn_div_small(x, n, 10000, &r);
        for (i = 0; i < 4; ++i) { d[nd++] = (char)('0' + r % 10); r /= 10; }
    }
    while (nd > 7 && d[nd - 1] == '0') --nd;    /* no leading zeros, but "0.xxxxxx" */
    while (nd < 7) d[nd++] = '0';
    for (i = nd; i-- > 6; ) buf[len++] = d[i];
    buf[len++] = '.';
    for (i = 6; i-- > 0; ) buf[len++] = d[i];
    return len;
}

/* --- toJSON's floats: nlohmann/json's to_chars --------------------------------------------- */
/* Nix prints a float in JSON with nlohmann/json, whose to_chars is Grisu2: it
 * always round-trips but is not always the shortest, so the VM ports it step
 * for step rather than print the shortest digits (docs/FLOAT.md). From
 * nlohmann/json 3.12.0 include/nlohmann/detail/conversions/to_chars.hpp,
 * under this notice:
 *
 *   SPDX-FileCopyrightText: 2009 Florian Loitsch <https://florian.loitsch.com/>
 *   SPDX-FileCopyrightText: 2013 - 2025 Niels Lohmann <https://nlohmann.me>
 *   SPDX-License-Identifier: MIT
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a
 *   copy of this software and associated documentation files (the
 *   "Software"), to deal in the Software without restriction, including
 *   without limitation the rights to use, copy, modify, merge, publish,
 *   distribute, sublicense, and/or sell copies of the Software, and to permit
 *   persons to whom the Software is furnished to do so, subject to the
 *   following conditions:
 *
 *   The above copyright notice and this permission notice shall be included
 *   in all copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 *   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
 *   NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 *   DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 *   OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 *   USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * A diyfp is f * 2^e, kept as two variables: a struct copy would link ~MOVE
 * into bank 0. */

/* The upper 64 bits of x * y, rounded (diyfp::mul; the exponent is the sum
 * plus 64). */
static uint64_t dfp_mul(uint64_t x, uint64_t y)
{
    uint64_t u_lo = x & 0xFFFFFFFFUL, u_hi = x >> 32, v_lo = y & 0xFFFFFFFFUL, v_hi = y >> 32;
    uint64_t p0 = u_lo * v_lo, p1 = u_lo * v_hi, p2 = u_hi * v_lo, p3 = u_hi * v_hi;
    uint64_t q = (p0 >> 32) + (p1 & 0xFFFFFFFFUL) + (p2 & 0xFFFFFFFFUL);
    q += (uint64_t)1 << 31;                     /* round, ties up */
    return p3 + (p2 >> 32) + (p1 >> 32) + (q >> 32);
}

/* get_cached_power_for_binary_exponent: c = f * 2^e ~= 10^-k, k returned.
 * The table is nlohmann's kCachedPowers, 10 bytes an entry (f, e; k is
 * -300 + 8i), in a literal: a literal stays with this code in its segment,
 * a static table would be bank 0's. k = f * 78913 / 2^18 (+1), C++'s
 * truncating division, done in shifts and unsigned: a signed 32-bit divide or
 * multiply would link a runtime routine into bank 0. */
static int16_t cached_power(int16_t e, uint64_t *cf, int16_t *ce)
{
    const uint8_t *t = (const uint8_t *)
        "\xCA\xC6\x9A\xC7\x17\xFE\x70\xAB\xDC\xFB\x4F\xDC\xBC\xBE\xFC\xB1\x77\xFF\xF6\xFB"
        "\x0C\xD6\x6B\x41\xEF\x91\x56\xBE\x11\xFC\x3C\xFC\x7F\x90\xAD\x1F\xD0\x8D\x2C\xFC"
        "\x83\x9A\x55\x31\x28\x5C\x51\xD3\x46\xFC\xB5\xC9\xA6\xAD\x8F\xAC\x71\x9D\x61\xFC"
        "\xCB\x8B\xEE\x23\x77\x22\x9C\xEA\x7B\xFC\x6D\x53\x78\x40\x91\x49\xCC\xAE\x96\xFC"
        "\x57\xCE\xB6\x5D\x79\x12\x3C\x82\xB1\xFC\x37\x56\xFB\x4D\x36\x94\x10\xC2\xCB\xFC"
        "\x4F\x98\x48\x38\x6F\xEA\x96\x90\xE6\xFC\xC7\x3A\x82\x25\xCB\x85\x74\xD7\x00\xFD"
        "\xF4\x97\xBF\x97\xCD\xCF\x86\xA0\x1B\xFD\xE5\xAC\x2A\x17\x98\x0A\x34\xEF\x35\xFD"
        "\x8E\xB2\x35\x2A\xFB\x67\x38\xB2\x50\xFD\x3B\x3F\xC6\xD2\xDF\xD4\xC8\x84\x6B\xFD"
        "\xBA\xCD\xD3\x1A\x27\x44\xDD\xC5\x85\xFD\x96\xC9\x25\xBB\xCE\x9F\x6B\x93\xA0\xFD"
        "\x84\xA5\x62\x7D\x24\x6C\xAC\xDB\xBA\xFD\xF6\xDA\x5F\x0D\x58\x66\xAB\xA3\xD5\xFD"
        "\x26\xF1\xC3\xDE\x93\xF8\xE2\xF3\xEF\xFD\xB8\x80\xFF\xAA\xA8\xAD\xB5\xB5\x0A\xFE"
        "\x8B\x4A\x7C\x6C\x05\x5F\x62\x87\x25\xFE\x53\x30\xC1\x34\x60\xFF\xBC\xC9\x3F\xFE"
        "\x55\x26\xBA\x91\x8C\x85\x4E\x96\x5A\xFE\xBD\x7E\x29\x70\x24\x77\xF9\xDF\x74\xFE"
        "\x8F\xB8\xE5\xB8\x9F\xBD\xDF\xA6\x8F\xFE\x94\x7D\x74\x88\xCF\x5F\xA9\xF8\xA9\xFE"
        "\xCF\x9B\xA8\x8F\x93\x70\x44\xB9\xC4\xFE\x6B\x15\x0F\xBF\xF8\xF0\x08\x8A\xDF\xFE"
        "\xB6\x31\x31\x65\x55\x25\xB0\xCD\xF9\xFE\xAC\x7F\x7B\xD0\xC6\xE2\x3F\x99\x14\xFF"
        "\x06\x3B\x2B\x2A\xC4\x10\x5C\xE4\x2E\xFF\xD3\x92\x73\x69\x99\x24\x24\xAA\x49\xFF"
        "\x0E\xCA\x00\x83\xF2\xB5\x87\xFD\x63\xFF\xEB\x1A\x11\x92\x64\x08\xE5\xBC\x7E\xFF"
        "\xCC\x88\x50\x6F\x09\xCC\xBC\x8C\x99\xFF\x2C\x65\x19\xE2\x58\x17\xB7\xD1\xB3\xFF"
        "\x00\x00\x00\x00\x00\x00\x40\x9C\xCE\xFF\x00\x00\x00\x00\x10\xA5\xD4\xE8\xE8\xFF"
        "\x00\x00\x62\xAC\xC5\xEB\x78\xAD\x03\x00\x84\x09\x94\xF8\x78\x39\x3F\x81\x1E\x00"
        "\xB3\x15\x07\xC9\x7B\xCE\x97\xC0\x38\x00\x70\x5C\xEA\x7B\xCE\x32\x7E\x8F\x53\x00"
        "\x68\x80\xE9\xAB\xA4\x38\xD2\xD5\x6D\x00\x45\x22\x9A\x17\x26\x27\x4F\x9F\x88\x00"
        "\x27\xFB\xC4\xD4\x31\xA2\x63\xED\xA2\x00\xA8\xAD\xC8\x8C\x38\x65\xDE\xB0\xBD\x00"
        "\xDB\x65\xAB\x1A\x8E\x08\xC7\x83\xD8\x00\x9A\x1D\x71\x42\xF9\x1D\x5D\xC4\xF2\x00"
        "\x58\xE7\x1B\xA6\x2C\x69\x4D\x92\x0D\x01\xEA\x8D\x70\x1A\x64\xEE\x01\xDA\x27\x01"
        "\x4A\x77\xEF\x9A\x99\xA3\x6D\xA2\x42\x01\x85\x6B\x7D\xB4\x7B\x78\x09\xF2\x5C\x01"
        "\x77\x18\xDD\x79\xA1\xE4\x54\xB4\x77\x01\xC2\xC5\x9B\x5B\x92\x86\x5B\x86\x92\x01"
        "\x3D\x5D\x96\xC8\xC5\x53\x35\xC8\xAC\x01\xB3\xA0\x97\xFA\x5C\xB4\x2A\x95\xC7\x01"
        "\xE3\x5F\xA0\x99\xBD\x9F\x46\xDE\xE1\x01\x25\x8C\x39\xDB\x34\xC2\x9B\xA5\xFC\x01"
        "\x5C\x9F\x98\xA3\x72\x9A\xC6\xF6\x16\x02\xCE\xBE\xE9\x54\x53\xBF\xDC\xB7\x31\x02"
        "\xE2\x41\x22\xF2\x17\xF3\xFC\x88\x4C\x02\xA5\x78\x5C\xD3\x9B\xCE\x20\xCC\x66\x02"
        "\xDF\x53\x21\x7B\xF3\x5A\x16\x98\x81\x02\x3A\x30\x1F\x97\xDC\xB5\xA0\xE2\x9B\x02"
        "\x96\xB3\xE3\x5C\x53\xD1\xD9\xA8\xB6\x02\x3C\x44\xA7\xA4\xD9\x7C\x9B\xFB\xD0\x02"
        "\x10\x44\xA4\xA7\x4C\x4C\x76\xBB\xEB\x02\x1A\x9C\x40\xB6\xEF\x8E\xAB\x8B\x06\x03"
        "\x2C\x84\x57\xA6\x10\xEF\x1F\xD0\x20\x03\x29\x31\x91\xE9\xE5\xA4\x10\x9B\x3B\x03"
        "\x9D\x0C\x9C\xA1\xFB\x9B\x10\xE7\x55\x03\x29\xF4\x3B\x62\xD9\x20\x28\xAC\x70\x03"
        "\x85\xCF\xA7\x7A\x5E\x4B\x44\x80\x8B\x03\x2D\xDD\xAC\x03\x40\xE4\x21\xBF\xA5\x03"
        "\x8F\xFF\x44\x5E\x2F\x9C\x67\x8E\xC0\x03\x41\xB8\x8C\x9C\x9D\x17\x33\xD4\xDA\x03"
        "\xA9\x1B\xE3\xB4\x92\xDB\x19\x9E\xF5\x03";
    int16_t f = -61 - e, k;
    uint32_t a, p;
    uint16_t i;
    a = (uint32_t)(f < 0 ? -f : f);
    p = (a << 16) + (a << 13) + (a << 12) + (a << 10) + (a << 6) + a;   /* a * 78913 */
    k = (int16_t)(p >> 18);
    if (f < 0) k = -k;
    else if (f > 0) ++k;
    i = (uint16_t)(300 + k + 7) >> 3;
    memcpy(cf, t + 10 * i, 8);
    memcpy(ce, t + 10 * i + 8, 2);
    return -300 + 8 * (int16_t)i;
}

static uint16_t largest_pow10(uint32_t n, uint32_t *pow10)   /* find_largest_pow10 */
{
    uint32_t p = 1000000000UL;
    uint16_t k = 10;
    while (k > 1 && n < p) { p /= 10; --k; }
    *pow10 = p;
    return k;
}

static void grisu2_round(char *buf, uint16_t len, uint64_t dist, uint64_t delta, uint64_t rest, uint64_t ten_k)
{
    while (rest < dist && delta - rest >= ten_k && (rest + ten_k < dist || dist - rest > rest + ten_k - dist)) {
        buf[len - 1]--;
        rest += ten_k;
    }
}

/* grisu2_digit_gen: the digits of V, M- <= V <= M+, all at the exponent e
 * (-60..-32); *dexp is adjusted. Its length. */
static uint16_t grisu2_digits(char *buf, int16_t *dexp, uint64_t mm, uint64_t w, uint64_t mp, int16_t e)
{
    uint64_t delta = mp - mm, dist = mp - w, one = shl(1, (uint16_t)-e), p2, rest, d;
    uint32_t p1, pow10, dd;
    uint16_t len = 0, n, m = 0, s = (uint16_t)-e;
    p1 = (uint32_t)shr(mp, s);
    p2 = mp & (one - 1);
    n = largest_pow10(p1, &pow10);
    while (n > 0) {
        dd = p1 / pow10;
        p1 %= pow10;
        buf[len++] = (char)('0' + dd);
        --n;
        rest = shl(p1, s) + p2;
        if (rest <= delta) {
            *dexp += (int16_t)n;
            grisu2_round(buf, len, dist, delta, rest, shl(pow10, s));
            return len;
        }
        pow10 /= 10;
    }
    for (;;) {
        p2 *= 10;
        d = shr(p2, s);
        p2 &= one - 1;
        buf[len++] = (char)('0' + (uint16_t)d);
        ++m;
        delta *= 10;
        dist *= 10;
        if (p2 <= delta) break;
    }
    *dexp -= (int16_t)m;
    grisu2_round(buf, len, dist, delta, p2, one);
    return len;
}

/* The e of a normalised f (top bit set): how far to shift f, which must not
 * be 0. */
static uint16_t norm(uint64_t *f)
{
    uint16_t z = clz64(*f);
    *f = shl(*f, z);
    return z;
}

/* compute_boundaries and grisu2: the digits of the positive finite a, and
 * *dexp such that a ~= digits * 10^dexp. */
static uint16_t grisu2(uint64_t a, char *buf, int16_t *dexp)
{
    int16_t E = F_EXP(a), ve, pe, me, ce;
    uint64_t F = F_FRAC(a), vf, pf, mf, cf, w, wm, wp;
    if (E == 0) { vf = F; ve = -1074; }
    else { vf = F + ((uint64_t)1 << 52); ve = E - 1075; }
    pf = 2 * vf + 1;                            /* m+ = (2v + 1) * 2^(e-1) */
    pe = ve - 1;
    if (F == 0 && E > 1) { mf = 4 * vf - 1; me = ve - 2; }   /* the lower boundary is closer */
    else { mf = 2 * vf - 1; me = ve - 1; }
    pe -= (int16_t)norm(&pf);                   /* w+ normalised */
    mf = shl(mf, (uint16_t)(me - pe));          /* w- to w+'s exponent */
    ve -= (int16_t)norm(&vf);                   /* w normalised: the same exponent as w+ */
    *dexp = -cached_power(pe, &cf, &ce);
    w  = dfp_mul(vf, cf);
    wm = dfp_mul(mf, cf);
    wp = dfp_mul(pf, cf);
    (void)ve;
    return grisu2_digits(buf, dexp, wm + 1, w, wp - 1, pe + ce + 64);
}

/* append_exponent: e, -1000 < e < 1000, as nlohmann writes it: a sign and
 * at least two digits. */
static uint16_t json_exp(char *buf, int16_t e)
{
    uint16_t k, n = 0;
    buf[n++] = e < 0 ? '-' : '+';
    k = (uint16_t)(e < 0 ? -e : e);
    if (k >= 100) { buf[n++] = (char)('0' + k / 100); k %= 100; buf[n++] = (char)('0' + k / 10); }
    else { buf[n++] = (char)('0' + k / 10); }
    buf[n++] = (char)('0' + k % 10);
    return n;
}

/* buf[at..at+n) moved up by by bytes, the overlap copied from the top down:
 * memmove would link into bank 0 (136 bytes). */
static void move_up(char *buf, uint16_t at, uint16_t n, uint16_t by)
{
    while (n--) buf[at + n + by] = buf[at + n];
}

/* format_buffer: lays out the k digits at buf, a ~= digits * 10^(n - k), as
 * nlohmann does -- fixed point for 10^-4 <= a < 10^15, else d.ddde+NN. Its
 * length. (One exit: with a return in each case, ORCA/C 2.3.0b1's optimizer
 * produced a branch the linker could not reach, "Relative address out of
 * range" -- TOOLCHAIN.md.) */
static uint16_t json_layout(char *buf, int16_t k, int16_t n)
{
    uint16_t i, len;
    if (k <= n && n <= 15) {                    /* digits[000].0 */
        memset(buf + k, '0', (size_t)(n - k));
        buf[n] = '.';
        buf[n + 1] = '0';
        len = (uint16_t)n + 2;
    } else if (0 < n && n <= 15) {              /* dig.its */
        move_up(buf, (uint16_t)n, (uint16_t)(k - n), 1);
        buf[n] = '.';
        len = (uint16_t)k + 1;
    } else if (-4 < n && n <= 0) {              /* 0.[000]digits */
        move_up(buf, 0, (uint16_t)k, (uint16_t)(2 - n));
        buf[0] = '0';
        buf[1] = '.';
        for (i = 0; i < (uint16_t)-n; ++i) buf[2 + i] = '0';
        len = 2 + (uint16_t)-n + (uint16_t)k;
    } else {
        if (k == 1) i = 1;                      /* dE+123 */
        else {                                  /* d.igitsE+123 */
            move_up(buf, 1, (uint16_t)(k - 1), 1);
            buf[1] = '.';
            i = (uint16_t)k + 1;
        }
        buf[i++] = 'e';
        len = i + json_exp(buf + i, n - 1);
    }
    return len;
}

/* nlohmann's dump_float of a double: "null" for an infinity or a NaN, else
 * to_chars -- a sign, "0.0" for a zero, else Grisu2's digits laid out by
 * format_buffer. Its length; buf holds F_JSON_MAX. */
uint16_t f_json(uint64_t a, char *buf)
{
    uint16_t s = 0, len;
    int16_t dexp;
    if (F_EXP(a) == 0x7FF) {
        memcpy(buf, "null", 4);
        len = 4;
    } else {
        if (F_SIGN(a)) { buf[s++] = '-'; a &= ~SIGN_BIT; }
        if (a == 0) {
            memcpy(buf + s, "0.0", 3);
            len = s + 3;
        } else {
            len = grisu2(a, buf + s, &dexp);
            len = s + json_layout(buf + s, (int16_t)len, (int16_t)len + dexp);
        }
    }
    return len;
}
