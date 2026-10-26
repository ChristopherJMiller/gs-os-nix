/* derivation.c -- derivationStrict and the derivation rule of equality
 * (docs/DERIVATION.md). Split from prims.c; what the three primop files share
 * is prims.h.
 */
#pragma noroot
#pragma optimize 15

#include "prims.h"

/* With prims.c, one load segment above bank 0 (VM.md). */
segment "prims";

/* The derivations this program made: [drvPath, modulo hash, next] lists, a
 * root (gc.c), for an input's modulo hash (DERIVATION.md "String context"). */
obj_t *drv_table;

/* --- derivations (docs/DERIVATION.md) ----------------------------------------------- */
#define DRV_OUT_MAX 8
#define FP_MAX      512         /* the longest output fingerprint: 7 + 209 + 8 + 64 + 12 + 211;
                                   a fixed output's longest, 19 + 128 + 1 + 255, is shorter */
#define NAME_MAX    211         /* StorePath::MaxPathLen */

enum { K_OTHER, K_NAME, K_IGNORE, K_STRUCT, K_CA, K_IMPURE, K_ARGS, K_BUILDER, K_SYSTEM,
       K_OUTPUTS, K_HASH, K_HALGO, K_HMODE };

/* The attribute names derivationStrict treats specially, matched against
 * literals, which ORCA/C keeps with the code: no bank 0. nm is a view. */
static uint8_t drv_key(obj_t *nm)
{
    if (IS_LIT(nm, "name"))               return K_NAME;
    if (IS_LIT(nm, "__ignoreNulls"))      return K_IGNORE;
    if (IS_LIT(nm, "__structuredAttrs"))  return K_STRUCT;
    if (IS_LIT(nm, "__contentAddressed")) return K_CA;
    if (IS_LIT(nm, "__impure"))           return K_IMPURE;
    if (IS_LIT(nm, "args"))               return K_ARGS;
    if (IS_LIT(nm, "builder"))            return K_BUILDER;
    if (IS_LIT(nm, "system"))             return K_SYSTEM;
    if (IS_LIT(nm, "outputs"))            return K_OUTPUTS;
    if (IS_LIT(nm, "outputHash"))         return K_HASH;
    if (IS_LIT(nm, "outputHashAlgo"))     return K_HALGO;
    if (IS_LIT(nm, "outputHashMode"))     return K_HMODE;
    return K_OTHER;
}

/* Entry i of the set (an original): its value, and its name in *nm. */
static obj_t *attr_at(obj_t *set, uint16_t i, obj_t **nm)
{
    obj_t *s = deref(set);
    uint16_t n = AUX16(s);
    *nm = ANAMES(s, n)[i];
    return AVALUES(s, n)[i];
}

/* Nix's checkName: 1..211 bytes of [A-Za-z0-9+-._?=], and not ".", "..", nor
 * starting ".-" or "..-". E_TYPE otherwise. */
static void check_name(const char *p, uint16_t n)
{
    uint16_t i;
    char c;
    if (n == 0 || n > NAME_MAX) type_err();
    if (p[0] == '.') {
        if (n == 1 || p[1] == '-') type_err();
        if (p[1] == '.' && (n == 2 || p[2] == '-')) type_err();
    }
    for (i = 0; i < n; ++i) {
        c = p[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              c == '+' || c == '-' || c == '.' || c == '_' || c == '?' || c == '='))
            type_err();
    }
}

/* The store-path name of output ov of the derivation named nmv (both views)
 * into pn: the name for "out", else name-ov. Its length; E_TYPE past 211. */
static uint16_t path_name(char *pn, obj_t *nmv, obj_t *ov)
{
    uint16_t nl = AUX16(nmv), ol = IS_LIT(ov, "out") ? 0 : AUX16(ov);
    if (nl > NAME_MAX || ol > NAME_MAX || (ol && nl + 1 + ol > NAME_MAX)) type_err();
    memcpy(pn, STRBYTES(nmv), nl);
    if (!ol) return nl;
    pn[nl] = '-';
    memcpy(pn + nl + 1, STRBYTES(ov), ol);
    return nl + 1 + ol;
}

/* Where phase 1 left everything. Stack indices, never pointers: phase 2
 * reserves, and a collection moves what they point at. */
struct drv {
    uint16_t at, np;            /* the (attribute, coerced string) pairs, vstack[at + 2i] */
    uint16_t ctx;               /* vstack index of the inputs: the union of every context */
    uint16_t on, no;            /* the output names, sorted, vstack[on + i] */
    uint16_t args;              /* rstack index of the coerced args LIST */
    uint16_t name, bld, sys;    /* pair indices */
    uint16_t nin;               /* elements in the inputs list */
    uint8_t masked;             /* output paths as "" (hashDerivationModulo) */
    uint8_t replaced;           /* input .drv paths as their modulo hashes */
    uint16_t fixed;             /* pair index of outputHash; 0xFFFF: not fixed-output */
    uint8_t fmeth, falgo;       /* its method (FM_*) and algorithm (HA_*) */
    uint8_t fh[64];             /* its hash, ha_size(falgo) bytes */
    obj_t *path[DRV_OUT_MAX];   /* phase 2: the output paths, young, and nothing collects */
};
#define PAIR_KEY(d, i) vstack[(d)->at + 2 * (i)]
#define PAIR_VAL(d, i) vstack[(d)->at + 2 * (i) + 1]
#define OUT_NAME(d, i) vstack[(d)->on + (i)]

/* An ATerm is written through a sink. A null p only counts. */
struct sink { uint8_t *p; uint16_t n, cap; };

static void put(struct sink *k, const char *s, uint16_t m)
{
    if ((uint16_t)(k->n + m) < k->n || k->n + m > k->cap) nomem();
    if (k->p) memcpy(k->p + k->n, s, m);
    k->n += m;
}

/* The STRING o (an original) in quotes: escaped as Nix's printString does,
 * or with esc 0 as its printUnquotedString does -- not at all. The ring may
 * turn over first, because no caller holds a view across this (DERIVATION.md
 * "Views"): a turnover here is as safe as at a bytecode boundary. */
static void put_str(struct sink *k, obj_t *o, uint8_t esc)
{
    uint16_t i, j, m;
    char c, e[2];
    const char *s;
    windows_maybe();
    o = deref(o);
    s = STRBYTES(o);
    m = AUX16(o);
    put(k, "\"", 1);
    for (i = j = 0; esc && i < m; ++i) {
        c = s[i];
        if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t') {
            put(k, s + j, i - j);
            e[0] = '\\';
            e[1] = c == '\n' ? 'n' : c == '\r' ? 'r' : c == '\t' ? 't' : c;
            put(k, e, 2);
            j = i + 1;
        }
    }
    put(k, s + j, m - j);
    put(k, "\"", 1);
}

/* --- fixed outputs (DERIVATION.md "Fixed outputs") ---
 * The hash is given, so nothing here computes md5, sha1 or sha512: every hash
 * the rule makes is a SHA-256. The decoders shift by constants only, because a
 * variable shift links ~SHIFTLEFT into bank 0. */
#define HA_NONE   0
#define HA_MD5    1
#define HA_SHA1   2
#define HA_SHA256 3
#define HA_SHA512 4
#define FM_FLAT   0
#define FM_NAR    1
#define IS_BYTES(p, n, lit) ((n) == sizeof(lit) - 1 && memcmp((p), (lit), (n)) == 0)

/* Nix's parseHashAlgoOpt: HA_NONE for a name it does not know, which Nix
 * drops in silence (measured); blake3 is an experimental feature. */
static uint8_t hash_algo(const char *p, uint16_t n)
{
    if (IS_BYTES(p, n, "md5"))    return HA_MD5;
    if (IS_BYTES(p, n, "sha1"))   return HA_SHA1;
    if (IS_BYTES(p, n, "sha256")) return HA_SHA256;
    if (IS_BYTES(p, n, "sha512")) return HA_SHA512;
    if (IS_BYTES(p, n, "blake3")) err_exit(E_UNSUP);
    return HA_NONE;
}

static uint16_t ha_size(uint8_t a)
{
    return a == HA_MD5 ? 16 : a == HA_SHA1 ? 20 : a == HA_SHA256 ? 32 : 64;
}

/* outputHashMode (a STRING view), as Nix's handleHashMode reads it: text and
 * git are experimental features. */
static uint8_t hash_mode(obj_t *s)
{
    if (IS_LIT(s, "flat")) return FM_FLAT;
    if (IS_LIT(s, "nar") || IS_LIT(s, "recursive")) return FM_NAR;
    if (IS_LIT(s, "text") || IS_LIT(s, "git")) err_exit(E_UNSUP);
    type_err();
    return 0;
}

static uint8_t hex_digit(char c)                /* either case; 16 if not one */
{
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
    return 16;
}

static uint8_t b64_digit(char c)                /* 64 if not one */
{
    if (c >= 'A' && c <= 'Z') return (uint8_t)(c - 'A');
    if (c >= 'a' && c <= 'z') return (uint8_t)(c - 'a' + 26);
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0' + 52);
    if (c == '+') return 62;
    if (c == '/') return 63;
    return 64;
}

/* Nix's newHashAllowEmpty: outputHash (the STRING view s) into d->fh. d->falgo
 * holds outputHashAlgo's algorithm (HA_NONE for none) on entry and the hash's
 * on return. E_TYPE wherever Nix rejects the hash. */
static void parse_hash(struct drv *d, obj_t *s)
{
    const char *p = STRBYTES(s);
    uint16_t n = AUX16(s), i, j, sz;
    uint8_t a, sri = 0, v, m, t, c = 0, *h = d->fh;
    memset(h, 0, 64);
    if (n == 0) {                               /* all zeros (Nix warns) */
        if (d->falgo == HA_NONE) type_err();
        return;
    }
    for (i = 0; i < n && p[i] != ':'; ++i) ;
    if (i == n) {
        for (i = 0; i < n && p[i] != '-'; ++i) ;
        sri = i < n;
    }
    if (i < n) {                                /* "<algo>:", or SRI's "<algo>-" */
        if ((a = hash_algo(p, i)) == HA_NONE) type_err();
        if (d->falgo != HA_NONE && d->falgo != a) type_err();
        p += i + 1;
        n -= i + 1;
    } else if ((a = d->falgo) == HA_NONE) {
        type_err();                             /* "does not include a type" */
    }
    d->falgo = a;
    sz = ha_size(a);
    if (!sri && n == 2 * sz) {                  /* base16 */
        for (i = 0; i < n; ++i) {
            if ((v = hex_digit(p[i])) == 16) type_err();
            h[i >> 1] |= i & 1 ? v : (uint8_t)(v << 4);
        }
    } else if (!sri && n == (8 * sz - 1) / 5 + 1) {
        /* nix32, from the end: the j-th digit from the end holds bits 5j..5j+4.
         * Bit by bit, so that a set bit past the last byte is the error Nix
         * makes it, and every shift is by one. */
        for (j = 0, i = 0, m = 1; j < n; ++j) {
            for (v = 0; v < 32 && "0123456789abcdfghijklmnpqrsvwxyz"[v] != p[n - 1 - j]; ++v) ;
            if (v == 32) type_err();
            for (t = 0; t < 5; ++t, v >>= 1) {
                if (v & 1) {
                    if (i == sz) type_err();
                    h[i] |= m;
                }
                if (!(m <<= 1)) { m = 1; ++i; }
            }
        }
    } else if (sri || n == (sz + 2) / 3 * 4) {
        /* Base64: it stops at the first "=", skips newlines, ignores the bits
         * left over, and must give exactly sz bytes. Four digits are three
         * bytes; c holds the bits not yet in one. */
        for (i = j = 0, t = 0; i < n && p[i] != '='; ++i) {
            if (p[i] == '\n') continue;
            if ((v = b64_digit(p[i])) == 64) type_err();
            if (t == 0) c = v;
            else {
                if (j == sz) type_err();
                if (t == 1)      { h[j++] = (uint8_t)(c << 2 | v >> 4); c = v & 15; }
                else if (t == 2) { h[j++] = (uint8_t)(c << 4 | v >> 2); c = v & 3; }
                else               h[j++] = (uint8_t)(c << 6 | v);
            }
            t = (t + 1) & 3;
        }
        if (j != sz) type_err();
    } else {
        type_err();                             /* "has wrong length" */
    }
}

/* "<m><algo>": the ATerm's second output field, "r:sha256", "sha1", ... */
static void put_method_algo(struct sink *k, struct drv *d)
{
    if (d->fmeth == FM_NAR) put(k, "r:", 2);
    if (d->falgo == HA_MD5) put(k, "md5", 3);
    else if (d->falgo == HA_SHA1) put(k, "sha1", 4);
    else if (d->falgo == HA_SHA256) put(k, "sha256", 6);
    else put(k, "sha512", 6);
}

static void put_hex(struct sink *k, const uint8_t *b, uint16_t n)
{
    char x[2];
    uint16_t i;
    for (i = 0; i < n; ++i) {
        hex_of(b + i, 1, x);
        put(k, x, 2);
    }
}

/* "fixed:out:<m><algo>:<hex>:": what makeFixedOutputPath hashes and, followed
 * by the output's path, what a fixed output's modulo hash is of. */
static void put_fixed(struct sink *k, struct drv *d)
{
    put(k, "fixed:out:", 10);
    put_method_algo(k, d);
    put(k, ":", 1);
    put_hex(k, d->fh, ha_size(d->falgo));
    put(k, ":", 1);
}

/* --- the inputs: the elements of the context union ---
 * Sorted by bytes, the "!<o>!<drv>" elements come first ('!' < '/'), then the
 * "/<path>" sources; "=<drv>" was refused. Each input is read through a KEY:
 * the .drv path, its modulo hash from drv_table, or the store path an element
 * refers to (the .drv, or the source). Keys are views, valid until the ring
 * turns over, which only happens where no key is held. */
#define KEY_DRV 0
#define KEY_HEX 1
#define KEY_REF 2

static int bytes_cmp(const char *a, uint16_t na, const char *b, uint16_t nb)
{
    int c = memcmp(a, b, na < nb ? na : nb);
    return c ? c : (int)na - (int)nb;
}

/* The derivation this program made whose .drv path is p[0..n): its modulo
 * hash, a view of a 64-character STRING. E_UNSUP if there is none -- a
 * context the program did not make. */
static obj_t *drv_hex(const char *p, uint16_t n)
{
    obj_t *t = drv_table, *tv, *pv;
    while (t) {
        tv = deref(t);                          /* [drvPath, hex, next] */
        pv = deref(ELEMS(tv)[0]);
        if (AUX16(pv) == n && memcmp(STRBYTES(pv), p, n) == 0) return deref(ELEMS(tv)[1]);
        t = ELEMS(tv)[2];
    }
    err_exit(E_UNSUP);
    return 0;
}

static obj_t *in_elem(struct drv *d, uint16_t j)       /* element j, a view */
{
    return deref(list_elem(vstack[d->ctx], j, d->nin));
}

static const char *in_key(struct drv *d, uint16_t j, uint8_t kind, uint16_t *len)
{
    obj_t *e = in_elem(d, j);
    const char *p = STRBYTES(e);
    uint16_t m = AUX16(e), i;
    if (p[0] != '!') { *len = m; return p; }            /* a source is itself */
    for (i = 1; p[i] != '!'; ++i) ;                     /* checked in phase 1 */
    if (kind == KEY_HEX) { e = drv_hex(p + i + 1, m - i - 1); *len = 64; return STRBYTES(e); }
    *len = m - i - 1;
    return p + i + 1;
}

/* The element whose key is the smallest greater than last's (-1: the
 * smallest of all): a selection sort, one step. Derivation inputs only, unless
 * the key is KEY_REF. Duplicate keys -- two outputs of one input -- come out
 * once. */
static int16_t in_next(struct drv *d, uint8_t kind, int16_t last)
{
    uint16_t j, lj, lb, ll;
    int16_t best = -1;
    const char *pj, *pb, *pl;
    windows_maybe();
    for (j = 0; j < d->nin; ++j) {
        if (kind != KEY_REF && STRBYTES(in_elem(d, j))[0] != '!') continue;
        pj = in_key(d, j, kind, &lj);
        if (last >= 0) { pl = in_key(d, (uint16_t)last, kind, &ll); if (bytes_cmp(pj, lj, pl, ll) <= 0) continue; }
        if (best >= 0) { pb = in_key(d, (uint16_t)best, kind, &lb); if (bytes_cmp(pj, lj, pb, lb) >= 0) continue; }
        best = (int16_t)j;
    }
    return best;
}

/* [("<drv>",["<o>",...]),...] by .drv path, or with each .drv replaced by its
 * modulo hash and ordered by that (hashDerivationModulo). */
static void put_input_drvs(struct sink *k, struct drv *d)
{
    uint8_t kind = d->replaced ? KEY_HEX : KEY_DRV, fo, first = 1;
    int16_t e = -1;
    uint16_t j, i, lk, lj;
    const char *pk, *pj, *o;
    while ((e = in_next(d, kind, e)) >= 0) {
        if (!first) put(k, ",", 1);
        first = 0;
        pk = in_key(d, (uint16_t)e, kind, &lk);
        put(k, "(\"", 2);
        put(k, pk, lk);
        put(k, "\",[", 3);
        for (j = 0, fo = 1; j < d->nin; ++j) {
            o = STRBYTES(in_elem(d, j));
            if (o[0] != '!') continue;
            pk = in_key(d, (uint16_t)e, kind, &lk);
            pj = in_key(d, j, kind, &lj);
            if (bytes_cmp(pk, lk, pj, lj) != 0) continue;
            o = STRBYTES(in_elem(d, j));        /* "!<o>!..." */
            for (i = 1; o[i] != '!'; ++i) ;
            if (!fo) put(k, ",", 1);
            fo = 0;
            put(k, "\"", 1);
            put(k, o + 1, i - 1);
            put(k, "\"", 1);
        }
        put(k, "])", 2);
    }
}

/* ["<src>",...]: the plain paths, already in order. */
static void put_input_srcs(struct sink *k, struct drv *d)
{
    uint16_t j, first = 1;
    for (j = 0; j < d->nin; ++j) {
        if (STRBYTES(in_elem(d, j))[0] == '!') continue;
        if (!first) put(k, ",", 1);
        first = 0;
        put_str(k, list_elem(vstack[d->ctx], j, d->nin), 0);
    }
}

/* ":<ref>" for every store path the .drv refers to, sorted, once each: the
 * type of its fingerprint is "text" followed by these. */
static void put_refs(struct sink *k, struct drv *d)
{
    int16_t e = -1;
    uint16_t l;
    const char *p;
    while ((e = in_next(d, KEY_REF, e)) >= 0) {
        p = in_key(d, (uint16_t)e, KEY_REF, &l);
        put(k, ":", 1);
        put(k, p, l);
    }
}

/* Derive([outputs],[inputs],[sources],system,builder,[args],[env]). */
static void aterm(struct sink *k, struct drv *d)
{
    uint16_t i, o, n, c;
    put(k, "Derive([", 8);
    for (o = 0; o < d->no; ++o) {
        if (o) put(k, ",", 1);
        put(k, "(", 1);
        put_str(k, OUT_NAME(d, o), 0);
        put(k, ",", 1);
        if (d->masked) put(k, "\"\"", 2); else put_str(k, d->path[o], 0);
        if (d->fixed == 0xFFFF) {
            put(k, ",\"\",\"\")", 7);
        } else {                                /* ,"<m><algo>","<hex>", masked or not */
            put(k, ",\"", 2);
            put_method_algo(k, d);
            put(k, "\",\"", 3);
            put_hex(k, d->fh, ha_size(d->falgo));
            put(k, "\")", 2);
        }
    }
    put(k, "],[", 3);
    put_input_drvs(k, d);
    put(k, "],[", 3);
    put_input_srcs(k, d);
    put(k, "],", 2);
    put_str(k, PAIR_VAL(d, d->sys), 0);         /* unescaped: measured (DERIVATION.md) */
    put(k, ",", 1);
    put_str(k, PAIR_VAL(d, d->bld), 1);
    put(k, ",[", 2);
    n = AUX16(deref(rstack[d->args]));
    for (i = 0; i < n; ++i) {
        if (i) put(k, ",", 1);
        put_str(k, list_elem(rstack[d->args], i, n), 1);
    }
    put(k, "],[", 3);
    /* The environment: the pairs and the outputs merged in key order, an
     * output replacing an attribute of the same name. */
    for (i = o = 0; i < d->np || o < d->no; ) {
        if (i == d->np) c = 2;
        else if (o == d->no) c = 0;
        else c = str_cmp(deref(PAIR_KEY(d, i)), deref(OUT_NAME(d, o)));
        if (i || o) put(k, ",", 1);
        put(k, "(", 1);
        if (c == 0) {
            put_str(k, PAIR_KEY(d, i), 1);
            put(k, ",", 1);
            put_str(k, PAIR_VAL(d, i), 1);
            ++i;
        } else {
            put_str(k, OUT_NAME(d, o), 1);
            put(k, ",", 1);
            if (d->masked) put(k, "\"\"", 2); else put_str(k, d->path[o], 1);
            if (c == 1) ++i;
            ++o;
        }
        put(k, ")", 1);
    }
    put(k, "])", 2);
}

static void sha256_any(const uint8_t *p, uint16_t n, uint8_t *dig)
{
    if (!card_sha256(p, n, dig)) sha256_cpu(p, n, dig);
}

/* makeStorePath: k holds the fingerprint's type ("output:<o>", "text:<refs>");
 * ":sha256:<hex>:/nix/store:<pn>" is appended and the whole hashed, the card's
 * P3 being the hash part (nix32_fold without a card). A young STRING
 * "/nix/store/<32>-<pn>". Nothing collects: the caller reserved. */
static obj_t *store_path(struct sink *k, const uint8_t *dig, const char *pn, uint16_t pl)
{
    char hx[64];
    uint8_t h[32];
    obj_t *r;
    put(k, ":sha256:", 8);
    hex_of(dig, 32, hx);
    put(k, hx, 64);
    put(k, ":/nix/store:", 12);
    put(k, pn, pl);
    r = str_alloc(44 + pl);
    memcpy(STRBYTES(r), "/nix/store/", 11);
    if (!card_storepath(k->p, k->n, (uint8_t *)STRBYTES(r) + 11)) {
        sha256_cpu(k->p, k->n, h);
        nix32_fold(h, STRBYTES(r) + 11);
    }
    STRBYTES(r)[43] = '-';
    memcpy(STRBYTES(r) + 44, pn, pl);
    return r;
}

/* A young one-element context, a LIST holding the STRING a ++ b: the results'
 * "=<drv>" and "!<o>!<drv>". Nothing collects: the caller reserved. */
static obj_t *ctx_one(const char *a, uint16_t na, const char *b, uint16_t nb)
{
    obj_t *e = str_alloc(na + nb), *l = list_alloc(1);
    memcpy(STRBYTES(e), a, na);
    memcpy(STRBYTES(e) + na, b, nb);
    ELEMS(l)[0] = e;
    return l;
}

/* Output names compare as byte strings. */
static int tok_cmp(const char *p, uint16_t a, uint16_t an, uint16_t b, uint16_t bn)
{
    return bytes_cmp(p + a, an, p + b, bn);
}

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/* derivationStrict attrs -> { drvPath; <output> = path; ... } (DERIVATION.md). */
void prim_derivationstrict(void)
{
    struct drv d;
    struct sink k;
    struct { uint16_t off, len; } tok[DRV_OUT_MAX];
    uint16_t b = rsp, n, i, j, m, key, pl, nl, dl, cnt = 0;
    uint16_t ixn = 0xFFFF, ixs = 0xFFFF, ixi = 0xFFFF, outs = 0xFFFF;
    uint8_t ignore = 0, hasdrv = 0, dig[32];
    uint32_t need, cap, grow = 0;
    char pn[NAME_MAX + 1];
    obj_t *so, *s, *v, *vo, *nm, *r, *e;
    const char *p;

    s = force_pop_as(T_ATTRS, &so);
    n = AUX16(s);
    rpush(so);                                  /* b: the set */
    rpush(list_alloc(0));                       /* b+1: the coerced args */
    d.args = b + 1;

    /* --- phase 1: evaluate. `name` first, then __structuredAttrs and
     * __ignoreNulls, as Nix forces them; then every attribute in name order. */
    for (i = 0; i < n; ++i) {
        windows_maybe();                        /* no view is held here */
        attr_at(rstack[b], i, &nm);
        key = drv_key(deref(nm));
        if (key == K_NAME) ixn = i; else if (key == K_STRUCT) ixs = i; else if (key == K_IGNORE) ixi = i;
    }
    if (ixn == 0xFFFF) err_exit(E_NOATTR);
    push(attr_at(rstack[b], ixn, &nm));
    v = force_pop_as(T_STRING, &vo);            /* forced, not coerced */
    if (CTX(v)) type_err();                     /* and without a context: forceStringNoCtx */
    if (ixs != 0xFFFF) {
        push(attr_at(rstack[b], ixs, &nm));
        v = force_pop_as(T_BOOL, &vo);
        if (BOOLVAL(v)) err_exit(E_UNSUP);
    }
    if (ixi != 0xFFFF) {
        push(attr_at(rstack[b], ixi, &nm));
        v = force_pop_as(T_BOOL, &vo);
        ignore = BOOLVAL(v);
    }
    d.at = vsp;
    d.np = 0;
    d.name = d.bld = d.sys = d.fixed = 0xFFFF;
    d.fmeth = FM_FLAT;
    d.falgo = HA_NONE;
    for (i = 0; i < n; ++i) {
        windows_maybe();
        v = attr_at(rstack[b], i, &nm);
        key = drv_key(deref(nm));
        if (key == K_IGNORE) continue;
        push(nm);                               /* the pair: the name, */
        push(v);                                /* and the value, to coerce */
        if (ignore) {
            force_top();
            if (deref(peek())->tag == T_NULL) { vsp -= 2; continue; }
        }
        switch (key) {
        case K_CA: case K_IMPURE:               /* consumed even when false (measured) */
            v = force_pop_as(T_BOOL, &vo);
            --vsp;
            if (BOOLVAL(v)) err_exit(E_UNSUP);
            continue;
        case K_ARGS:
            v = force_pop_as(T_LIST, &vo);
            --vsp;
            m = AUX16(v);
            rstack[b + 1] = vo;                 /* the source, rooted while it is coerced */
            for (j = 0; j < m; ++j) {
                push(RELEM(b + 1, j, m));
                coerce_top(CO_DRV);
            }
            rstack[b + 1] = list_from_stack(m);
            continue;
        case K_NAME:    d.name = d.np; break;
        case K_BUILDER: d.bld = d.np; break;
        case K_SYSTEM:  d.sys = d.np; break;
        case K_OUTPUTS: outs = d.np; break;
        case K_HASH:    d.fixed = d.np; break;
        }
        coerce_top(CO_DRV);
        if (key == K_HALGO)                     /* read as soon as coerced, as Nix does */
            { s = deref(peek()); d.falgo = hash_algo(STRBYTES(s), AUX16(s)); }
        else if (key == K_HMODE)
            d.fmeth = hash_mode(deref(peek()));
        ++d.np;
    }
    if (d.bld == 0xFFFF || AUX16(deref(PAIR_VAL(&d, d.bld))) == 0) err_exit(E_NOATTR);
    if (d.sys == 0xFFFF || AUX16(deref(PAIR_VAL(&d, d.sys))) == 0) err_exit(E_NOATTR);

    /* The inputs: the union of every coerced string's context -- the pairs
     * (whose names have none) and the arguments, pushed for the moment. */
    m = AUX16(deref(rstack[b + 1]));
    for (i = 0; i < m; ++i) push(list_elem(rstack[b + 1], i, m));
    v = ctx_join(d.at, 2 * d.np + m);
    vsp -= m;
    d.ctx = vsp;
    push(v);
    d.nin = v ? AUX16(deref(v)) : 0;
    for (i = 0; i < d.nin; ++i) {
        windows_maybe();
        e = in_elem(&d, i);
        p = STRBYTES(e);
        m = AUX16(e);
        if (p[0] == '!') {                      /* "!<o>!<drv>": an output of a derivation */
            for (j = 1; j < m && p[j] != '!'; ++j) ;
            if (j == 1 || j + 1 >= m) err_exit(E_UNSUP);
            drv_hex(p + j + 1, m - j - 1);      /* one this program made, or E_UNSUP */
            hasdrv = 1;
        } else if (p[0] != '/') {
            err_exit(E_UNSUP);                  /* "=<drv>": its closure (DERIVATION.md) */
        }
    }

    /* The outputs: `outputs` split on whitespace, sorted, or just "out". */
    d.on = vsp;
    if (outs == 0xFFFF) {
        r = str_alloc(3);                       /* inside the boundary's margin */
        memcpy(STRBYTES(r), "out", 3);
        push(r);
        d.no = 1;
    } else {
        s = deref(PAIR_VAL(&d, outs));
        p = STRBYTES(s);
        m = AUX16(s);
        d.no = 0;
        for (i = 0; ; ) {
            while (i < m && is_ws(p[i])) ++i;
            if (i == m) break;
            j = i;
            while (i < m && !is_ws(p[i])) ++i;
            if (d.no == DRV_OUT_MAX) err_exit(E_UNSUP);
            tok[d.no].off = j;
            tok[d.no].len = i - j;
            ++d.no;
        }
        if (d.no == 0) type_err();              /* "an empty set of outputs" */
        for (i = 1; i < d.no; ++i)              /* insertion sort */
            for (j = i; j && tok_cmp(p, tok[j - 1].off, tok[j - 1].len, tok[j].off, tok[j].len) > 0; --j) {
                m = tok[j].off; tok[j].off = tok[j - 1].off; tok[j - 1].off = m;   /* not a struct copy: */
                m = tok[j].len; tok[j].len = tok[j - 1].len; tok[j - 1].len = m;   /* that links ~MOVE into bank 0 */
            }
        for (i = 0, need = 0; i < d.no; ++i) {
            if (tok[i].len == 7 && memcmp(p + tok[i].off, "drvPath", 7) == 0) type_err();
            if (i && tok_cmp(p, tok[i - 1].off, tok[i - 1].len, tok[i].off, tok[i].len) == 0) type_err();
            need += round4(tok[i].len) + 8;
        }
        if (need > NURSERY_END) nomem();
        nursery_reserve((uint16_t)need);
        s = deref(PAIR_VAL(&d, outs));
        for (i = 0; i < d.no; ++i) {
            r = str_alloc(tok[i].len);
            memcpy(STRBYTES(r), STRBYTES(s) + tok[i].off, tok[i].len);
            push(r);
        }
    }

    /* Names: `name` must not end in .drv; name.drv and each output's path
     * name must be store-path names. */
    s = deref(PAIR_VAL(&d, d.name));
    nl = AUX16(s);
    check_name(STRBYTES(s), nl);                /* first: it bounds nl */
    if (nl + 4 > NAME_MAX) type_err();          /* name.drv */
    if (nl >= 4 && memcmp(STRBYTES(s) + nl - 4, ".drv", 4) == 0) type_err();
    dl = 44 + nl + 4;                           /* the .drv's path */
    if (d.fixed != 0xFFFF) {                    /* fixed-output: exactly "out", and the hash */
        if (d.no != 1 || !IS_LIT(deref(OUT_NAME(&d, 0)), "out")) type_err();
        parse_hash(&d, deref(PAIR_VAL(&d, d.fixed)));
    }
    /* The reserve. The scratch buffer takes, in turn, the masked ATerm (inputs
     * replaced), each output's fingerprint, the final ATerm (real inputs), the
     * .drv's fingerprint and, with inputs, the final ATerm with them replaced
     * again: as long as the longest. Two measuring passes give the masked
     * lengths both ways; the outputs add two copies of each path. A fixed
     * output has no masked ATerm, but its fields are printed masked or not, so
     * the same sum is its final ATerm's length; its fingerprints fit FP_MAX. */
    d.masked = 1;
    d.replaced = 1;
    k.p = 0; k.n = 0; k.cap = 0xFFFF;
    aterm(&k, &d);
    cap = k.n;
    d.replaced = 0;
    k.n = 0;
    aterm(&k, &d);
    if (k.n > cap) cap = k.n;
    k.n = 0;
    put_refs(&k, &d);
    need = (round4(7) + 8)                      /* "drvPath" */
         + (round4(dl) + 8)                     /* the .drv's path */
         + (round4(1 + dl) + 8 + 8)             /* its context, "=<drv>" in a LIST */
         + (16 + 72)                            /* its drv_table entry and modulo hash */
         + (4 + 12 * (d.no + 1));               /* the result set */
    for (i = 0; i < d.no; ++i) {
        pl = path_name(pn, deref(PAIR_VAL(&d, d.name)), deref(OUT_NAME(&d, i)));
        check_name(pn, pl);
        grow += 2 * (44 + pl);                  /* the path, in the outputs and the environment */
        need += round4(44 + pl) + 8;            /* the path */
        need += round4(2 + AUX16(deref(OUT_NAME(&d, i))) + dl) + 8 + 8;   /* its "!<o>!<drv>" */
    }
    cap += grow;
    if (cap < FP_MAX) cap = FP_MAX;
    if (cap < (uint32_t)k.n + 4 + 8 + 64 + 12 + nl + 4) cap = (uint32_t)k.n + 4 + 8 + 64 + 12 + nl + 4;
    if (cap > NURSERY_END - 16) nomem();        /* round4 is 16 bits */
    need += round4((uint16_t)cap) + 8;
    if (need > NURSERY_END) nomem();
    nursery_reserve((uint16_t)need);

    /* --- phase 2: serialise and hash. No bytecode; nothing moves. */
    r = str_alloc((uint16_t)cap);
    k.p = (uint8_t *)STRBYTES(r);
    k.cap = (uint16_t)cap;
    if (d.fixed != 0xFFFF) {                    /* makeFixedOutputPath: from the hash, never the ATerm */
        pl = path_name(pn, deref(PAIR_VAL(&d, d.name)), deref(OUT_NAME(&d, 0)));
        k.n = 0;
        if (d.falgo == HA_SHA256 && d.fmeth == FM_NAR) {
            put(&k, "source", 6);               /* the hash is the fingerprint's own */
            d.path[0] = store_path(&k, d.fh, pn, pl);
        } else {
            put_fixed(&k, &d);
            sha256_any(k.p, k.n, dig);
            k.n = 0;
            put(&k, "output:out", 10);
            d.path[0] = store_path(&k, dig, pn, pl);
        }
    } else {
        d.masked = 1;
        d.replaced = 1;
        k.n = 0;
        aterm(&k, &d);
        sha256_any(k.p, k.n, dig);              /* hashDerivationModulo */
        for (i = 0; i < d.no; ++i) {
            pl = path_name(pn, deref(PAIR_VAL(&d, d.name)), deref(OUT_NAME(&d, i)));
            s = deref(OUT_NAME(&d, i));
            k.n = 0;
            put(&k, "output:", 7);
            put(&k, STRBYTES(s), AUX16(s));
            d.path[i] = store_path(&k, dig, pn, pl);
        }
    }
    d.masked = 0;
    d.replaced = 0;
    k.n = 0;
    aterm(&k, &d);                              /* final */
    sha256_any(k.p, k.n, dig);
    s = deref(PAIR_VAL(&d, d.name));
    memcpy(pn, STRBYTES(s), nl);
    memcpy(pn + nl, ".drv", 4);
    k.n = 0;
    put(&k, "text", 4);
    put_refs(&k, &d);                           /* "text:<every input .drv and source>" */
    v = store_path(&k, dig, pn, nl + 4);
    if (d.fixed != 0xFFFF) {                    /* a fixed output's modulo hash: the hash and */
        k.n = 0;                                /* the path, whatever the inputs */
        put_fixed(&k, &d);
        put(&k, STRBYTES(d.path[0]), AUX16(d.path[0]));
        sha256_any(k.p, k.n, dig);
    } else if (hasdrv) {                        /* the modulo hash for dependents: */
        d.replaced = 1;                         /* final, but with the inputs replaced */
        k.n = 0;
        aterm(&k, &d);
        sha256_any(k.p, k.n, dig);
    }                                           /* (with none, the final ATerm's) */
    nm = str_alloc(64);
    hex_of(dig, 32, STRBYTES(nm));
    e = list_alloc(3);
    ELEMS(e)[0] = v;
    ELEMS(e)[1] = nm;
    ELEMS(e)[2] = drv_table;
    drv_table = e;
    /* The results' contexts: the .drv as a whole, and each output of it. */
    STRCTX(v, dl) = ctx_one("=", 1, STRBYTES(v), dl);
    for (i = 0; i < d.no; ++i) {
        s = deref(OUT_NAME(&d, i));
        pn[0] = '!';
        memcpy(pn + 1, STRBYTES(s), AUX16(s));  /* an output name is at most 209 bytes */
        pn[1 + AUX16(s)] = '!';
        e = d.path[i];
        STRCTX(e, AUX16(e)) = ctx_one(pn, 2 + AUX16(s), STRBYTES(v), dl);
    }
    r = attrs_alloc(d.no + 1);
    nm = str_alloc(7);
    memcpy(STRBYTES(nm), "drvPath", 7);
    insert_entry(r, d.no + 1, &cnt, nm, v);
    for (i = 0; i < d.no; ++i) insert_entry(r, d.no + 1, &cnt, OUT_NAME(&d, i), d.path[i]);
    vsp = d.at;
    rsp = b;
    push(r);
}

/* Nix's isDerivation: the set (an original) has a `type` that forces to the
 * string "derivation". Runs bytecode. */
static int is_drv(obj_t *set)
{
    uint16_t ix;
    obj_t *t;
    if (!find_attr(set, str_type, &ix, &t)) return 0;
    push(t);
    force_top();
    t = deref_pop();
    return t->tag == T_STRING && IS_LIT(t, "derivation");
}

/* values_equal for two sets, a and b (originals, forced, not the same object).
 * Nix's rule first: if both are derivations and both have an outPath, they
 * are equal iff the outPaths are, whatever else differs -- d == d // { x = 1; }
 * is true. Otherwise name by name and value by value. Runs bytecode. It lives
 * here, not in data.c with the other cases, because bank 0 is full. */
int attrs_equal(obj_t *a, obj_t *b)
{
    uint16_t base = rsp, i, n, ix;
    int r = 1;
    obj_t *sa, *sb, *pa, *pb;
    rpush(a);
    rpush(b);
    if (is_drv(rstack[base]) && is_drv(rstack[base + 1]) &&
        find_attr(rstack[base], str_outpath, &ix, &pa) &&
        find_attr(rstack[base + 1], str_outpath, &ix, &pb)) {
        r = values_equal(pa, pb);               /* find_attr does not collect: pa is still good */
    } else {
        n = AUX16(deref(rstack[base]));
        if (AUX16(deref(rstack[base + 1])) != n) r = 0;
        for (i = 0; r && i < n; ++i) {
            sa = deref(rstack[base]);
            sb = deref(rstack[base + 1]);
            if (!str_eq(deref(ANAMES(sa, n)[i]), deref(ANAMES(sb, n)[i]))) r = 0;
            else if (!values_equal(AVALUES(sa, n)[i], AVALUES(sb, n)[i])) r = 0;
        }
    }
    rsp = base;
    return r;
}
