/* sha256.c -- SHA-256 on the 65816, for when the card cannot: no card at all,
 * or the verb masked so checks.verb-economics can price HASH_PUSH/HASH_STREAM
 * against it (vm.h VERB_NO_HASH). FIPS 180-4, straightforwardly.
 *
 * It is slow: ORCA/C calls a library routine for every 32-bit shift and
 * rotate (~300 cycles each; vm.h BYTE_OF), and a block has hundreds of them.
 * That is the price the card's hasher removes, and the D5 gate reports it; a
 * hand-written assembly version would be several times faster and still lose
 * by orders of magnitude to a verb whose cost is ~55 bus accesses a message
 * (CARD-ISA.md, the HASH_STREAM micro-benchmark).
 */
#pragma noroot
#pragma optimize 15

#include "vm.h"

segment "prims";

static const uint32_t K[64] = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL, 0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
    0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL, 0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
    0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL, 0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
    0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL, 0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
    0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL, 0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
    0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL, 0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
    0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL, 0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
    0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL, 0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

/* H and W live on the C stack, not in statics: ORCA/C puts every static in
 * bank 0 whatever segment its code is in (the symbol map shows it), and bank 0
 * is the one that is nearly full. K has to be a static; 256 bytes. */
static void block(uint32_t *H, const uint8_t *p)
{
    uint32_t W[64];
    uint32_t a, b, c, d, e, f, g, h, t1, t2;
    uint16_t i;
    for (i = 0; i < 16; ++i)
        W[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
               ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
    for (i = 16; i < 64; ++i) {
        uint32_t s0 = ROTR(W[i - 15], 7) ^ ROTR(W[i - 15], 18) ^ (W[i - 15] >> 3);
        uint32_t s1 = ROTR(W[i - 2], 17) ^ ROTR(W[i - 2], 19) ^ (W[i - 2] >> 10);
        W[i] = W[i - 16] + s0 + W[i - 7] + s1;
    }
    a = H[0]; b = H[1]; c = H[2]; d = H[3]; e = H[4]; f = H[5]; g = H[6]; h = H[7];
    for (i = 0; i < 64; ++i) {
        t1 = h + (ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + W[i];
        t2 = (ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    H[0] += a; H[1] += b; H[2] += c; H[3] += d; H[4] += e; H[5] += f; H[6] += g; H[7] += h;
}

void sha256_cpu(const uint8_t *src, uint16_t n, uint8_t *out)
{
    uint8_t buf[64];
    uint32_t H[8];
    uint16_t i, left = n;
    uint32_t bits = (uint32_t)n << 3;           /* n < 64 KB: the length fits 32 bits */
    H[0] = 0x6a09e667UL; H[1] = 0xbb67ae85UL; H[2] = 0x3c6ef372UL; H[3] = 0xa54ff53aUL;
    H[4] = 0x510e527fUL; H[5] = 0x9b05688cUL; H[6] = 0x1f83d9abUL; H[7] = 0x5be0cd19UL;
    while (left >= 64) { block(H, src); src += 64; left -= 64; }
    memset(buf, 0, 64);
    memcpy(buf, src, left);
    buf[left] = 0x80;
    if (left >= 56) { block(H, buf); memset(buf, 0, 64); }
    buf[60] = BYTE_OF(bits, 3); buf[61] = BYTE_OF(bits, 2);   /* big-endian length; the top */
    buf[62] = BYTE_OF(bits, 1); buf[63] = BYTE_OF(bits, 0);   /* four bytes stay zero */
    block(H, buf);
    for (i = 0; i < 8; ++i) {
        out[4 * i]     = BYTE_OF(H[i], 3);
        out[4 * i + 1] = BYTE_OF(H[i], 2);
        out[4 * i + 2] = BYTE_OF(H[i], 1);
        out[4 * i + 3] = BYTE_OF(H[i], 0);
    }
}

/* What the card's HASH_FINAL leaves in P3 (storepath.v), for when there is no
 * card: the digest XOR-folded to 20 bytes (f[i % 20] ^= h[i]), then Nix's
 * base32, which emits from the END -- character i is bits 5(31-i) upward of
 * the folded bytes read as one little-endian number. 32 characters. */
void nix32_fold(const uint8_t *dig, char *out)
{
    uint8_t f[20];
    uint16_t i, b, j, w;
    for (i = 0; i < 20; ++i) f[i] = dig[i];
    for (i = 20; i < 32; ++i) f[i - 20] ^= dig[i];
    for (i = 0; i < 32; ++i) {
        b = 5 * (31 - i);
        j = b >> 3;
        w = f[j] | (j < 19 ? (uint16_t)f[j + 1] << 8 : 0);
        /* A signed shift, which ORCA/C already links (~SSHIFTRIGHT); an unsigned
         * or left variable shift would link another routine into bank 0. The
         * sign cannot reach the 5 bits kept: they come from bits <= 11. */
        out[i] = "0123456789abcdfghijklmnpqrsvwxyz"[((int)w >> (b & 7)) & 31];
    }
}
