/* float-vectors.c -- test vectors for target/vm/c/float.c, with the HOST's
 * answers (D7): its own IEEE-754 hardware for the arithmetic, its own glibc
 * printf("%f") for toString -- the same libc Nix prints with, so even a NaN's
 * "-nan" is the host's. Writes a C file that target/tests/float_test.c, built
 * by ORCA/C and run on a2min, checks float.c against (checks.float-vectors).
 *
 * Usage: float-vectors SEED JSONBITS PART > float_vectors.c
 * PART is "arith" (the arithmetic and %f vectors), "json" (toJSON's text) or
 * "parse" (fromJSON's numbers, answered by the host's strtod): one ORCA/C
 * test binary cannot hold all their data below $C000, so they are three,
 * and the random stream is the same for both, so a seed names the same
 * vectors. JSONBITS gets the toJSON vectors' bit patterns, one hexadecimal
 * per line, for scripts/json-floats.cpp to answer with nlohmann/json (the
 * library Nix prints JSON with); float-vectors.sh appends the answers.
 * The operands are random bit patterns weighted toward the classes where
 * soft-float goes wrong: zeros of both signs, subnormals, the largest finite,
 * infinities, NaN, values near 1 (so sums cancel and round), and halfway
 * cases for %f. The first N_TIE arithmetic vectors are exact ties, which
 * random operands almost never are: x + half an ulp of x, at several
 * magnitudes, both signs, odd and even significands, and a subnormal result,
 * so rounding ties to even is tested every run.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_EQ, OP_LT, OP_I2F, OP_FLOOR, OP_CEIL, OP_N };
#define N_ARITH 1000
#define N_TIE   96
#define N_FMT   120
#define N_JSON  400
#define N_PARSE 300
#define N_G     200

static uint64_t s;
static uint64_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
static uint64_t bits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static double val(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }

static uint64_t operand(void)
{
    static const uint64_t edge[] = {
        0, 0x8000000000000000ULL, 1, 0x8000000000000001ULL, 0x000FFFFFFFFFFFFFULL,
        0x0010000000000000ULL, 0x7FEFFFFFFFFFFFFFULL, 0xFFEFFFFFFFFFFFFFULL,
        0x7FF0000000000000ULL, 0xFFF0000000000000ULL, 0xFFF8000000000000ULL,
        0x3FF0000000000000ULL, 0xBFF0000000000000ULL, 0x4000000000000000ULL,
        0x3FE0000000000000ULL, 0x3CA0000000000000ULL, 0x4340000000000000ULL,
        0x43E0000000000000ULL, 0xC3E0000000000000ULL };
    uint64_t r = rnd(), u;
    switch (r % 6) {
    case 0: return edge[(r >> 8) % (sizeof edge / sizeof edge[0])];
    case 1: return (r & 0x800FFFFFFFFFFFFFULL) | ((uint64_t)(0x3F0 + rnd() % 32) << 52);
    case 2: return rnd() & 0x800FFFFFFFFFFFFFULL;
    default:
        u = rnd();
        if (((u >> 52) & 0x7FF) == 0x7FF && (u & 0xFFFFFFFFFFFFFULL)) u = 0xFFF8000000000000ULL;
        return u;
    }
}

static int emit = 1;                    /* print this part, or only draw from the stream */
#define P if (emit) printf

static void bytes(uint64_t u)
{
    int i;
    for (i = 0; i < 8; ++i) P("%u,", (unsigned)((u >> (8 * i)) & 0xFF));
}

int main(int argc, char **argv)
{
    int i, op;
    uint64_t a, b, r;
    double x, y, f;
    int64_t v;
    char str[400];
    uint64_t fa[N_FMT], ja[N_JSON], pa[N_PARSE], ga[N_G];
    /* %g's ties: integers whose seventh digit is a 5 exactly, and halves */
    const double gspecial[] = { 1234565.0, 1234575.0, 9999995.0, 100000.5, 100001.5, 2.5, 0.5, 999999.5, 999998.5,
                                123456.5, 123457.5, 0.0001, 0.00001, 1e100, 5e-324, 1.7976931348623157e308, -0.0,
                                1.0 / 3, 1e6, 1e5, 123456789.0, 4.35, 0.000123456789 };
    FILE *jf;
    int json = argc > 3 && strcmp(argv[3], "json") == 0, parse = argc > 3 && strcmp(argv[3], "parse") == 0;
    const char *pspecial[] = { "0", "-0", "1", "1.5", "0.1", "1e400", "1e-400", "4.9e-324", "2.4703282292062327e-324",
        "2.4703282292062328e-324", "2.2250738585072012e-308", "2.2250738585072011e-308", "1.7976931348623157e308",
        "1.7976931348623158e308", "1.7976931348623159e308", "9007199254740993", "9007199254740995",
        "123456789012345678901234567890", "0.30000000000000004", "-0e0", "1E-2", "5e-324", "0.000001", "1e23",
        "8.98846567431158e307", "2.2250738585072014e-308", "4.4501477170144023e-308", "7.2057594037927933e16" };
    char pb[128], *pe;
    int k, np;
    long double mid;
    const double jspecial[] = { 0.0, -0.0, 1.0, 1.5, 0.1, 1e21, 1e-7, 123456789.0, 0.3, 1e15, 1e16, 1e17,
                                1e-5, 1e-4, 1e-6, 1.0 / 3, 100.0, 9007199254740992.0, 123456789012345680.0,
                                1e100, 1.7976931348623157e308, 2.2250738585072014e-308, 5e-324, 1e-310,
                                999999999999999.9, 1e14, 0.0001234, 12345.678, -2.5, 4.35, 0.30000000000000004 };
    const uint64_t special[] = { 0, 0x8000000000000000ULL, 0x7FF0000000000000ULL, 0xFFF0000000000000ULL,
                                 0xFFF8000000000000ULL, 0x7FEFFFFFFFFFFFFFULL, 1, bits(0.0000005),
                                 bits(0.0000015), bits(0.0000025), bits(1e30), bits(-1e-9) };
    s = 0x9E3779B97F4A7C15ULL ^ strtoull(argc > 1 ? argv[1] : "1", 0, 10);
    printf("/* generated by scripts/float-vectors.c, seed %s: the host's answers */\n", argc > 1 ? argv[1] : "1");
    printf("#pragma noroot\n#include <stdint.h>\n");
    printf("const uint16_t n_arith = %d, n_fmt = %d, n_json = %d, n_parse = %d, n_g = %d;\n", json || parse ? 0 : N_ARITH,
           json || parse ? 0 : N_FMT, json ? N_JSON : 0, parse ? N_PARSE : 0, json ? N_G : 0);
    if (json || parse) printf("const uint8_t arith[1][25] = { { 0 } };\nconst uint8_t fmt_bits[1][8] = { { 0 } };\n"
                              "const char *const fmt_want[1] = { \"\" };\n");
    emit = !json && !parse;
    P("const uint8_t arith[%d][25] = {\n", N_ARITH);
    for (i = 0; i < N_ARITH; ++i) {
        op = (int)(rnd() % OP_N);
        a = operand();
        b = operand();
        if (i < N_TIE) {                         /* x + ulp(x)/2: a tie */
            int e = (int)(rnd() % 2000) + 2 + (i % 8 == 0 ? -100000 : 0);
            if (e < 1) e = 1 + (int)(rnd() % 60);   /* small exponents: subnormal halves */
            a = ((uint64_t)e << 52) | (rnd() & 0xFFFFFFFFFFFFFULL);
            if (i & 1) a |= 0x8000000000000000ULL;
            x = val(a);
            y = nextafter(x, x > 0 ? INFINITY : -INFINITY) - x;    /* one ulp, exactly */
            b = bits(y / 2);
            op = (i & 2) ? OP_SUB : OP_ADD;
            if (op == OP_SUB) b ^= 0x8000000000000000ULL;          /* x - (-ulp/2) */
        }
        x = val(a);
        y = val(b);
        switch (op) {
        case OP_ADD: r = bits(x + y); break;
        case OP_SUB: r = bits(x - y); break;
        case OP_MUL: r = bits(x * y); break;
        case OP_DIV: if (y == 0) b = bits(y = 3.0); r = bits(x / y); break;
        case OP_EQ:  r = x == y; break;
        case OP_LT:  r = x < y; break;
        case OP_I2F: v = (int64_t)rnd() >> (rnd() % 64); a = (uint64_t)v; r = bits((double)v); break;
        default:
            f = op == OP_CEIL ? ceil(x) : floor(x);
            if (isnan(f) || f < -9223372036854775808.0 || f >= 9223372036854775808.0) r = 0, b = 0;   /* out of range */
            else r = (uint64_t)(int64_t)f, b = 1;
        }
        P("{%d,", op);
        bytes(a);
        bytes(b);
        bytes(r);
        P("},\n");
    }
    P("};\nconst uint8_t fmt_bits[%d][8] = {\n", N_FMT);
    for (i = 0; i < N_FMT; ++i) {
        fa[i] = i < (int)(sizeof special / sizeof special[0]) ? special[i] : operand();
        P("{");
        bytes(fa[i]);
        P("},\n");
    }
    P("};\nconst char *const fmt_want[%d] = {\n", N_FMT);
    for (i = 0; i < N_FMT; ++i) {
        snprintf(str, sizeof str, "%f", val(fa[i]));
        P("\"%s\",\n", str);
    }
    P("};\n");
    if (!json) printf("const uint8_t json_bits[1][8] = { { 0 } };\n"   /* and json_want, from the script */
                      "const uint8_t g_bits[1][8] = { { 0 } };\nconst char *const g_want[1] = { \"\" };\n");
    emit = json;
    P("const uint8_t json_bits[%d][8] = {\n", N_JSON);
    jf = fopen(argc > 2 ? argv[2] : "/dev/null", "w");
    if (!jf) return 1;
    for (i = 0; i < N_JSON; ++i) {
        if (i < (int)(sizeof jspecial / sizeof jspecial[0])) ja[i] = bits(jspecial[i]);
        else if (i < (int)(sizeof jspecial / sizeof jspecial[0]) + 4) ja[i] = special[2 + (i & 3)];   /* infinities, NaN */
        else ja[i] = operand();
        P("{");
        bytes(ja[i]);
        P("},\n");
        if (json) fprintf(jf, "%016llx\n", (unsigned long long)ja[i]);
    }
    fclose(jf);
    P("};\n");
    /* toXML's floats: printf("%g"), which is ostream's << (with the toJSON part) */
    P("const uint8_t g_bits[%d][8] = {\n", N_G);
    for (i = 0; i < N_G; ++i) {
        ga[i] = i < (int)(sizeof gspecial / sizeof gspecial[0]) ? bits(gspecial[i]) : operand();
        P("{");
        bytes(ga[i]);
        P("},\n");
    }
    P("};\nconst char *const g_want[%d] = {\n", N_G);
    for (i = 0; i < N_G; ++i) {
        snprintf(str, sizeof str, "%g", val(ga[i]));
        P("\"%s\",\n", str);
    }
    P("};\n");
    /* fromJSON's numbers: decimal strings in JSON's grammar, and the host's strtod */
    emit = parse;
    if (!parse) printf("const char *const parse_text[1] = { \"0\" };\nconst uint8_t parse_bits[1][8] = { { 0 } };\n");
    P("const char *const parse_text[%d] = {\n", N_PARSE);
    for (i = 0; i < N_PARSE; ++i) {
        if (i < (int)(sizeof pspecial / sizeof pspecial[0])) { strcpy(pb, pspecial[i]); }
        else if (i < (int)(sizeof pspecial / sizeof pspecial[0]) + 60) {
            /* halfway between a double and the next, exactly (x86's long double
             * holds it, and glibc prints it exactly): a tie, which goes to even,
             * then the same with a 1 after its last digit, just above, which
             * goes up -- where only the remainder of strtod's division decides */
            x = ldexp(1.0 + (double)(rnd() >> 12) / 4503599627370496.0, (int)(rnd() % 41) - 20);
            mid = (long double)x + ((long double)nextafter(x, INFINITY) - (long double)x) / 2;
            snprintf(pb, sizeof pb, "%.75Le", mid);
            pe = strchr(pb, 'e');
            np = (int)(pe - pb);
            while (pb[np - 1] == '0') --np;
            if (i & 1) pb[np++] = '1';
            memmove(pb + np, pe, strlen(pe) + 1);
        } else {
            np = 0;
            if (rnd() & 1) pb[np++] = '-';
            k = 1 + (int)(rnd() % (rnd() % 4 ? 18 : 40));
            if (rnd() % 5 == 0) { pb[np++] = '0'; pb[np++] = '.'; while (rnd() % 3) pb[np++] = '0'; }
            pb[np++] = (char)('1' + rnd() % 9);
            while (--k > 0) {
                pb[np++] = (char)('0' + rnd() % 10);
                if (k == 3 && !strchr(pb, '.') && rnd() % 2) pb[np++] = '.';
            }
            if (pb[np - 1] == '.') pb[np++] = '0';
            if (rnd() % 2) np += sprintf(pb + np, "%c%s%d", rnd() % 2 ? 'e' : 'E', rnd() % 3 ? "" : (rnd() % 2 ? "+" : "-"),
                                         (int)(rnd() % 340));
            pb[np] = 0;
        }
        P("\"%s\",\n", pb);
        x = strtod(pb, 0);
        pa[i] = bits(x);
    }
    P("};\nconst uint8_t parse_bits[%d][8] = {\n", N_PARSE);
    for (i = 0; i < N_PARSE; ++i) { P("{"); bytes(pa[i]); P("},\n"); }
    P("};\n");
    return 0;
}
