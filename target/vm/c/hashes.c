/* hashes.c -- builtins.hashString's md5, sha1 and sha512 on the 65816. The
 * card hashes only SHA-256 (sha256.c is its fallback); these three are RFC
 * 1321's MD5 and FIPS 180-4's SHA-1 and SHA-512, straightforwardly.
 *
 * Every table is a string literal read through a cast. A literal stays with
 * its code (checked in cvm.bin), where a static array is bank 0's whatever
 * segment defines it, and the 65816 is little-endian, so each literal holds
 * its constants' native bytes (generated, and checked against the standards'
 * first and last values). The message is read a block at a time -- from the
 * nursery, or across the bus for a promoted string -- so a string of any
 * length is hashed, not only one a window can hold.
 */
#pragma noroot
#pragma optimize 15

#include "prims.h"

segment "hashes";

/* MD5's T[i] = floor(|sin(i + 1)| * 2^32), its rotations by round, and the
 * message word each step reads */
#define MD5_K \
    "\x78\xa4\x6a\xd7\x56\xb7\xc7\xe8\xdb\x70\x20\x24\xee\xce\xbd\xc1" \
    "\xaf\x0f\x7c\xf5\x2a\xc6\x87\x47\x13\x46\x30\xa8\x01\x95\x46\xfd" \
    "\xd8\x98\x80\x69\xaf\xf7\x44\x8b\xb1\x5b\xff\xff\xbe\xd7\x5c\x89" \
    "\x22\x11\x90\x6b\x93\x71\x98\xfd\x8e\x43\x79\xa6\x21\x08\xb4\x49" \
    "\x62\x25\x1e\xf6\x40\xb3\x40\xc0\x51\x5a\x5e\x26\xaa\xc7\xb6\xe9" \
    "\x5d\x10\x2f\xd6\x53\x14\x44\x02\x81\xe6\xa1\xd8\xc8\xfb\xd3\xe7" \
    "\xe6\xcd\xe1\x21\xd6\x07\x37\xc3\x87\x0d\xd5\xf4\xed\x14\x5a\x45" \
    "\x05\xe9\xe3\xa9\xf8\xa3\xef\xfc\xd9\x02\x6f\x67\x8a\x4c\x2a\x8d" \
    "\x42\x39\xfa\xff\x81\xf6\x71\x87\x22\x61\x9d\x6d\x0c\x38\xe5\xfd" \
    "\x44\xea\xbe\xa4\xa9\xcf\xde\x4b\x60\x4b\xbb\xf6\x70\xbc\xbf\xbe" \
    "\xc6\x7e\x9b\x28\xfa\x27\xa1\xea\x85\x30\xef\xd4\x05\x1d\x88\x04" \
    "\x39\xd0\xd4\xd9\xe5\x99\xdb\xe6\xf8\x7c\xa2\x1f\x65\x56\xac\xc4" \
    "\x44\x22\x29\xf4\x97\xff\x2a\x43\xa7\x23\x94\xab\x39\xa0\x93\xfc" \
    "\xc3\x59\x5b\x65\x92\xcc\x0c\x8f\x7d\xf4\xef\xff\xd1\x5d\x84\x85" \
    "\x4f\x7e\xa8\x6f\xe0\xe6\x2c\xfe\x14\x43\x01\xa3\xa1\x11\x08\x4e" \
    "\x82\x7e\x53\xf7\x35\xf2\x3a\xbd\xbb\xd2\xd7\x2a\x91\xd3\x86\xeb"
#define MD5_S \
    "\x07\x0c\x11\x16\x05\x09\x0e\x14\x04\x0b\x10\x17\x06\x0a\x0f\x15"
#define MD5_G \
    "\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f" \
    "\x01\x06\x0b\x00\x05\x0a\x0f\x04\x09\x0e\x03\x08\x0d\x02\x07\x0c" \
    "\x05\x08\x0b\x0e\x01\x04\x07\x0a\x0d\x00\x03\x06\x09\x0c\x0f\x02" \
    "\x00\x07\x0e\x05\x0c\x03\x0a\x01\x08\x0f\x06\x0d\x04\x0b\x02\x09"

/* SHA-512's K, the cube roots of the first 80 primes, and its H0, the square
 * roots of the first 8 (fractional parts, 64 bits each) */
#define SHA512_K \
    "\x22\xae\x28\xd7\x98\x2f\x8a\x42\xcd\x65\xef\x23\x91\x44\x37\x71" \
    "\x2f\x3b\x4d\xec\xcf\xfb\xc0\xb5\xbc\xdb\x89\x81\xa5\xdb\xb5\xe9" \
    "\x38\xb5\x48\xf3\x5b\xc2\x56\x39\x19\xd0\x05\xb6\xf1\x11\xf1\x59" \
    "\x9b\x4f\x19\xaf\xa4\x82\x3f\x92\x18\x81\x6d\xda\xd5\x5e\x1c\xab" \
    "\x42\x02\x03\xa3\x98\xaa\x07\xd8\xbe\x6f\x70\x45\x01\x5b\x83\x12" \
    "\x8c\xb2\xe4\x4e\xbe\x85\x31\x24\xe2\xb4\xff\xd5\xc3\x7d\x0c\x55" \
    "\x6f\x89\x7b\xf2\x74\x5d\xbe\x72\xb1\x96\x16\x3b\xfe\xb1\xde\x80" \
    "\x35\x12\xc7\x25\xa7\x06\xdc\x9b\x94\x26\x69\xcf\x74\xf1\x9b\xc1" \
    "\xd2\x4a\xf1\x9e\xc1\x69\x9b\xe4\xe3\x25\x4f\x38\x86\x47\xbe\xef" \
    "\xb5\xd5\x8c\x8b\xc6\x9d\xc1\x0f\x65\x9c\xac\x77\xcc\xa1\x0c\x24" \
    "\x75\x02\x2b\x59\x6f\x2c\xe9\x2d\x83\xe4\xa6\x6e\xaa\x84\x74\x4a" \
    "\xd4\xfb\x41\xbd\xdc\xa9\xb0\x5c\xb5\x53\x11\x83\xda\x88\xf9\x76" \
    "\xab\xdf\x66\xee\x52\x51\x3e\x98\x10\x32\xb4\x2d\x6d\xc6\x31\xa8" \
    "\x3f\x21\xfb\x98\xc8\x27\x03\xb0\xe4\x0e\xef\xbe\xc7\x7f\x59\xbf" \
    "\xc2\x8f\xa8\x3d\xf3\x0b\xe0\xc6\x25\xa7\x0a\x93\x47\x91\xa7\xd5" \
    "\x6f\x82\x03\xe0\x51\x63\xca\x06\x70\x6e\x0e\x0a\x67\x29\x29\x14" \
    "\xfc\x2f\xd2\x46\x85\x0a\xb7\x27\x26\xc9\x26\x5c\x38\x21\x1b\x2e" \
    "\xed\x2a\xc4\x5a\xfc\x6d\x2c\x4d\xdf\xb3\x95\x9d\x13\x0d\x38\x53" \
    "\xde\x63\xaf\x8b\x54\x73\x0a\x65\xa8\xb2\x77\x3c\xbb\x0a\x6a\x76" \
    "\xe6\xae\xed\x47\x2e\xc9\xc2\x81\x3b\x35\x82\x14\x85\x2c\x72\x92" \
    "\x64\x03\xf1\x4c\xa1\xe8\xbf\xa2\x01\x30\x42\xbc\x4b\x66\x1a\xa8" \
    "\x91\x97\xf8\xd0\x70\x8b\x4b\xc2\x30\xbe\x54\x06\xa3\x51\x6c\xc7" \
    "\x18\x52\xef\xd6\x19\xe8\x92\xd1\x10\xa9\x65\x55\x24\x06\x99\xd6" \
    "\x2a\x20\x71\x57\x85\x35\x0e\xf4\xb8\xd1\xbb\x32\x70\xa0\x6a\x10" \
    "\xc8\xd0\xd2\xb8\x16\xc1\xa4\x19\x53\xab\x41\x51\x08\x6c\x37\x1e" \
    "\x99\xeb\x8e\xdf\x4c\x77\x48\x27\xa8\x48\x9b\xe1\xb5\xbc\xb0\x34" \
    "\x63\x5a\xc9\xc5\xb3\x0c\x1c\x39\xcb\x8a\x41\xe3\x4a\xaa\xd8\x4e" \
    "\x73\xe3\x63\x77\x4f\xca\x9c\x5b\xa3\xb8\xb2\xd6\xf3\x6f\x2e\x68" \
    "\xfc\xb2\xef\x5d\xee\x82\x8f\x74\x60\x2f\x17\x43\x6f\x63\xa5\x78" \
    "\x72\xab\xf0\xa1\x14\x78\xc8\x84\xec\x39\x64\x1a\x08\x02\xc7\x8c" \
    "\x28\x1e\x63\x23\xfa\xff\xbe\x90\xe9\xbd\x82\xde\xeb\x6c\x50\xa4" \
    "\x15\x79\xc6\xb2\xf7\xa3\xf9\xbe\x2b\x53\x72\xe3\xf2\x78\x71\xc6" \
    "\x9c\x61\x26\xea\xce\x3e\x27\xca\x07\xc2\xc0\x21\xc7\xb8\x86\xd1" \
    "\x1e\xeb\xe0\xcd\xd6\x7d\xda\xea\x78\xd1\x6e\xee\x7f\x4f\x7d\xf5" \
    "\xba\x6f\x17\x72\xaa\x67\xf0\x06\xa6\x98\xc8\xa2\xc5\x7d\x63\x0a" \
    "\xae\x0d\xf9\xbe\x04\x98\x3f\x11\x1b\x47\x1c\x13\x35\x0b\x71\x1b" \
    "\x84\x7d\x04\x23\xf5\x77\xdb\x28\x93\x24\xc7\x40\x7b\xab\xca\x32" \
    "\xbc\xbe\xc9\x15\x0a\xbe\x9e\x3c\x4c\x0d\x10\x9c\xc4\x67\x1d\x43" \
    "\xb6\x42\x3e\xcb\xbe\xd4\xc5\x4c\x2a\x7e\x65\xfc\x9c\x29\x7f\x59" \
    "\xec\xfa\xd6\x3a\xab\x6f\xcb\x5f\x17\x58\x47\x4a\x8c\x19\x44\x6c"
#define SHA512_H \
    "\x08\xc9\xbc\xf3\x67\xe6\x09\x6a\x3b\xa7\xca\x84\x85\xae\x67\xbb" \
    "\x2b\xf8\x94\xfe\x72\xf3\x6e\x3c\xf1\x36\x1d\x5f\x3a\xf5\x4f\xa5" \
    "\xd1\x82\xe6\xad\x7f\x52\x0e\x51\x1f\x6c\x3e\x2b\x8c\x68\x05\x9b" \
    "\x6b\xbd\x41\xfb\xab\xd9\x83\x1f\x79\x21\x7e\x13\x19\xcd\xe0\x5b"

/* SHA-1's H0; MD5's is its first four words */
#define SHA1_H \
    "\x01\x23\x45\x67\x89\xab\xcd\xef\xfe\xdc\xba\x98\x76\x54\x32\x10\xf0\xe1\xd2\xc3"

#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define ROTR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

/* Big-endian words by moving bytes, not shifting them: a 32-bit shift is a
 * ~300-cycle library call (vm.h BYTE_OF) */
static uint32_t be32(const uint8_t *p)
{
    uint32_t w;
    uint8_t *q = (uint8_t *)&w;
    q[0] = p[3]; q[1] = p[2]; q[2] = p[1]; q[3] = p[0];
    return w;
}

static uint64_t be64(const uint8_t *p)
{
    uint64_t w;
    uint8_t *q = (uint8_t *)&w;
    uint16_t i;
    for (i = 0; i < 8; ++i) q[i] = p[7 - i];
    return w;
}

static void md5_block(uint32_t *H, const uint8_t *p)
{
    const uint32_t *X = (const uint32_t *)p;    /* little-endian words, as the 65816's */
    const uint32_t *K = (const uint32_t *)MD5_K;
    const uint8_t *R = (const uint8_t *)MD5_S, *G = (const uint8_t *)MD5_G;
    uint32_t a = H[0], b = H[1], c = H[2], d = H[3], f, t;
    uint16_t i, s;
    for (i = 0; i < 64; ++i) {
        if (i < 16) f = (b & c) | (~b & d);
        else if (i < 32) f = (d & b) | (~d & c);
        else if (i < 48) f = b ^ c ^ d;
        else f = c ^ (b | ~d);
        t = a + f + K[i] + X[G[i]];
        s = R[((i >> 4) << 2) | (i & 3)];
        a = d; d = c; c = b;
        b += ROTL32(t, s);
    }
    H[0] += a; H[1] += b; H[2] += c; H[3] += d;
}

/* The schedule is 16 words, rolling, not 80: the C stack is ~7 KB */
static void sha1_block(uint32_t *H, const uint8_t *p)
{
    uint32_t W[16], a = H[0], b = H[1], c = H[2], d = H[3], e = H[4], f, k, t;
    uint16_t i;
    for (i = 0; i < 16; ++i) W[i] = be32(p + 4 * i);
    for (i = 0; i < 80; ++i) {
        if (i >= 16) {
            t = W[(i + 13) & 15] ^ W[(i + 8) & 15] ^ W[(i + 2) & 15] ^ W[i & 15];
            W[i & 15] = ROTL32(t, 1);
        }
        if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5a827999UL; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ed9eba1UL; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdcUL; }
        else             { f = b ^ c ^ d;                   k = 0xca62c1d6UL; }
        t = ROTL32(a, 5) + f + e + k + W[i & 15];
        e = d; d = c; c = ROTL32(b, 30); b = a; a = t;
    }
    H[0] += a; H[1] += b; H[2] += c; H[3] += d; H[4] += e;
}

static void sha512_block(uint64_t *H, const uint8_t *p)
{
    const uint64_t *K = (const uint64_t *)SHA512_K;
    uint64_t W[16], a = H[0], b = H[1], c = H[2], d = H[3], e = H[4], f = H[5], g = H[6], h = H[7];
    uint64_t t1, t2, x, y;
    uint16_t i;
    for (i = 0; i < 16; ++i) W[i] = be64(p + 8 * i);
    for (i = 0; i < 80; ++i) {
        if (i >= 16) {
            x = W[(i + 1) & 15];
            y = W[(i + 14) & 15];
            W[i & 15] += (ROTR64(x, 1) ^ ROTR64(x, 8) ^ (x >> 7)) + W[(i + 9) & 15] +
                         (ROTR64(y, 19) ^ ROTR64(y, 61) ^ (y >> 6));
        }
        t1 = h + (ROTR64(e, 14) ^ ROTR64(e, 18) ^ ROTR64(e, 41)) + ((e & f) ^ (~e & g)) + K[i] + W[i & 15];
        t2 = (ROTR64(a, 28) ^ ROTR64(a, 34) ^ ROTR64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    H[0] += a; H[1] += b; H[2] += c; H[3] += d; H[4] += e; H[5] += f; H[6] += g; H[7] += h;
}

static void block(uint8_t algo, uint32_t *st, const uint8_t *p)
{
    if (algo == HS_MD5) md5_block(st, p);
    else if (algo == HS_SHA1) sha1_block(st, p);
    else sha512_block((uint64_t *)st, p);
}

/* n bytes of the STRING so (an original) from byte off: from the nursery, or
 * across the bus for a promoted one */
static void msg_bytes(obj_t *so, uint16_t off, uint8_t *dst, uint16_t n)
{
    if (IS_MATURE(so)) card_read_at(PBITS(so) + 4 + off, dst, n);
    else memcpy(dst, STRBYTES(deref(so)) + off, n);
}

/* The digest of a forced STRING original (HS_MD5 16 bytes, HS_SHA1 20,
 * HS_SHA512 64). Allocates nothing and runs no bytecode. */
void str_hash(obj_t *so, uint8_t algo, uint8_t *dig)
{
    uint8_t buf[128];
    uint32_t st[16];                            /* md5's 4 words, sha1's 5, sha512's 8 of 64 bits */
    obj_t hdr, *s;
    uint16_t n, off, left, i, bs = algo == HS_SHA512 ? 128 : 64;
    uint32_t bits;
    if (IS_MATURE(so)) { card_read_at(PBITS(so), &hdr, 4); s = &hdr; }
    else s = deref(so);
    need(s, T_STRING);
    n = AUX16(s);
    if (algo == HS_SHA512) memcpy(st, SHA512_H, 64);
    else memcpy(st, SHA1_H, 20);
    for (off = 0; (uint16_t)(n - off) >= bs; off += bs) {
        msg_bytes(so, off, buf, bs);
        block(algo, st, buf);
    }
    left = n - off;
    msg_bytes(so, off, buf, left);
    buf[left] = 0x80;                           /* then zeros, then the length in bits */
    memset(buf + left + 1, 0, bs - left - 1);
    if (left >= bs - (algo == HS_SHA512 ? 16 : 8)) {
        block(algo, st, buf);
        memset(buf, 0, bs);
    }
    bits = (uint32_t)n << 3;                    /* n < 64 KB: three bytes of it */
    if (algo == HS_MD5) {                       /* little-endian at 56 */
        buf[56] = BYTE_OF(bits, 0); buf[57] = BYTE_OF(bits, 1); buf[58] = BYTE_OF(bits, 2);
    } else {                                    /* big-endian, ending the block */
        buf[bs - 1] = BYTE_OF(bits, 0); buf[bs - 2] = BYTE_OF(bits, 1); buf[bs - 3] = BYTE_OF(bits, 2);
    }
    block(algo, st, buf);
    /* md5's words little-endian, as they are; sha1's and sha512's big-endian */
    if (algo == HS_MD5) memcpy(dig, st, 16);
    else if (algo == HS_SHA1)
        for (i = 0; i < 20; ++i) dig[i] = ((uint8_t *)st)[(i & ~3U) + 3 - (i & 3)];
    else
        for (i = 0; i < 64; ++i) dig[i] = ((uint8_t *)st)[(i & ~7U) + 7 - (i & 7)];
}
