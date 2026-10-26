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
