/* strings.c -- string primops, coercion, interpolation and string context
 * (BYTECODE.md "Primops", DERIVATION.md "String context"). Split from prims.c;
 * what the three primop files share is prims.h.
 */
#pragma noroot
#pragma optimize 15

#include "prims.h"

/* With prims.c, one load segment above bank 0 (VM.md). */
segment "prims";

/* --- strings ---------------------------------------------------------------------------- */
void prim_stringlength(void)
{
    obj_t *o, *s;
    coerce_top(CO_PLAIN);                       /* coerced, as Nix does: a set via __toString / outPath */
    s = force_pop_as(T_STRING, &o);
    push(new_int(AUX16(s)));
}

void prim_substring(void)                       /* start len s */
{
    uint16_t start, len = 0, slen, end;
    int to_end = 0;
    obj_t *so, *s, *vo, *v, *r;
    coerce_top(CO_PLAIN);                       /* the string argument is coerced (a set via __toString) */
    s = force_pop_as(T_STRING, &so);
    int64_t x;
    rpush(so);
    v = force_pop_as(T_INT, &vo);               /* len: negative means "to the end" */
    x = INTVAL(v);
    if (x < 0 || x > 0xFFFF) to_end = 1; else len = (uint16_t)x;
    v = force_pop_as(T_INT, &vo);               /* start */
    x = INTVAL(v);
    if (x < 0) err_exit(E_INDEX);
    s = deref(rstack[rsp - 1]);                 /* still rooted, for the reserve */
    slen = AUX16(s);
    if (x > 0xFFFF || (uint16_t)x >= slen) {   /* "", with the string's context, as Nix keeps it */
        r = str_alloc(0);
        STRCTX(r, 0) = CTX(s);
        --rsp;
        push(r);
        return;
    }
    start = (uint16_t)x;
    if (to_end || (uint16_t)(start + len) < start || start + len > slen) end = slen;
    else end = start + len;
    nursery_reserve_n(1, round4(end - start), 8);
    s = deref_rpop();
    r = str_alloc(end - start);
    memcpy(STRBYTES(r), STRBYTES(s) + start, end - start);
    STRCTX(r, end - start) = CTX(s);
    push(r);
}

/* The INT v as a decimal STRING. */
static obj_t *int_to_str(int64_t v)
{
    char buf[24];
    uint16_t n = 0, i;
    obj_t *o;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    /* 64-bit % and / are ORCA/C's ~UDIV8, ~4,000 cycles each: two per digit
     * made toString 37% of a program that prints numbers (a2min's function
     * profile, eval-okay-big-operations). Almost every number printed fits 16
     * bits, so the narrow loops come first and the wide one runs only above. */
    if (u <= 0xFFFFU) {
        uint16_t w = (uint16_t)u;
        do { buf[n++] = (char)('0' + w % 10); w /= 10; } while (w);
    } else if (u <= 0xFFFFFFFFUL) {
        uint32_t w = (uint32_t)u;
        do { buf[n++] = (char)('0' + (uint16_t)(w % 10)); w /= 10; } while (w);
    } else {
        do { buf[n++] = (char)('0' + (uint16_t)(u % 10)); u /= 10; } while (u);
    }
    if (v < 0) buf[n++] = '-';
    o = str_alloc(n);
    for (i = 0; i < n; ++i) STRBYTES(o)[i] = buf[n - 1 - i];
    return o;
}

static void concat_strings_sep_top(void);

/* The value on top of the stack is replaced by its STRING form: Nix's
 * coerceToString. Runs bytecode.
 *
 * A set coerces through `__toString` (called with the set itself) if it has
 * one, else through `outPath`, and the result is coerced again by the same
 * rules -- which is how `"${drv}"` becomes a store path, and why
 * { __toString = _: true; } is "1" under toString.
 *
 * `how` (vm.h CO_*): CO_MORE is coerceMore -- INTs, BOOLs, null and lists
 * coerce too, a list as its elements joined by spaces (so a throwing element
 * propagates, catchably). CO_PATH makes a path its text, as builtins.toString
 * does; without it a path would be copied into the store, which needs the
 * store (L7), so it is E_UNSUP rather than a string without its context.
 * Without CO_MORE, the rules of `+` and "${}": strings and coercible sets. */
void coerce_top(uint8_t how)
{
    uint16_t b = rsp, n, i, ix, at;
    uint8_t space;
    obj_t *vo, *v = 0, *f;
    vo = force_pop_view(&v);
    if (v->tag == T_ATTRS) {
        if (find_attr(vo, str_tostring, &ix, &f)) {
            push(f);                            /* f self */
            push(vo);
            do_call();
        } else if (find_attr(vo, str_outpath, &ix, &f)) {
            push(f);
        } else {
            type_err();
        }
        coerce_top(how);
        return;
    }
    if (v->tag == T_STRING) { push(vo); return; }
    if (v->tag == T_PATH) {                     /* its text, as a string: no store copy */
        if (!(how & CO_PATH)) err_exit(E_UNSUP);
        rpush(vo);
        nursery_reserve_n(1, round4(AUX16(v)), 8);
        v = deref_rpop();
        vo = str_alloc(AUX16(v));
        memcpy(STRBYTES(vo), STRBYTES(v), AUX16(v));
        push(vo);
        return;
    }
    if (!(how & CO_MORE)) type_err();
    switch (v->tag) {
    case T_INT:    push(int_to_str(INTVAL(v))); return;
    case T_FLOAT:  push(float_str(v)); return;     /* %f: "4.200000" (docs/FLOAT.md) */
    case T_BOOL:   if (BOOLVAL(v)) push(str_one); else push(str_alloc(0)); return;
    case T_NULL:   push(str_alloc(0)); return;
    case T_LIST:   break;
    default:       type_err();
    }
    /* The pieces go on the stack -- each element coerced, and a space after it
     * -- and are joined with nothing between. Nix puts no space after an element
     * that is an EMPTY LIST (its own comment: "not quite correct"), so
     * toString [ [] "a" [] [] "b" [ [] ] "c" ] is "a b  c". Joining with " "
     * gave " a   b  c". */
    n = AUX16(v);
    rpush(vo);                                  /* source */
    at = vsp;
    for (i = 0; i < n; ++i) {
        push(RELEM(b, i, n));
        force_top();
        v = deref(peek());
        space = i + 1 < n && !(v->tag == T_LIST && AUX16(v) == 0);
        coerce_top(how);
        if (space) push(str_space);
    }
    rsp = b;
    v = list_from_stack(vsp - at);
    push(str_alloc(0));                         /* inside the boundary's margin; v is young and nothing collects */
    push(v);
    concat_strings_sep_top();
}

void prim_tostring(void) { coerce_top(CO_STRING); }

/* n bytes as 2n lowercase hex digits. Arithmetic rather than a table: a
 * static table is bank 0 (STATUS.md). */
void hex_of(const uint8_t *dig, uint16_t n, char *out)
{
    uint16_t i, d;
    for (i = 0; i < 2 * n; ++i) {
        d = i & 1 ? dig[i >> 1] & 15 : dig[i >> 1] >> 4;
        out[i] = (char)(d < 10 ? '0' + d : 'a' - 10 + d);
    }
}

int is_lit(obj_t *s, const char *lit, uint16_t n)          /* a STRING view */
{
    return AUX16(s) == n && memcmp(STRBYTES(s), lit, n) == 0;
}

/* The SHA-256 of the forced STRING original so, where it lives: a mature one
 * stays on the card and HASH_STREAM reads it there (only its 4-byte header
 * crosses the bus), a young one goes through HASH_PUSH, and with no card or
 * the verb masked sha256.c does it on the CPU. Allocates nothing. */
void str_sha256(obj_t *so, uint8_t *dig)
{
    uint16_t n;
    obj_t *s, hdr;
    if (IS_MATURE(so)) {
        card_read_at(PBITS(so), &hdr, 4);
        need(&hdr, T_STRING);
        n = AUX16(&hdr);
        if (!card_sha256_at(PBITS(so) + 4, n, dig)) { s = deref(so); sha256_cpu((uint8_t *)STRBYTES(s), n, dig); }
    } else {
        s = deref(so);
        need(s, T_STRING);
        n = AUX16(s);
        if (!card_sha256((uint8_t *)STRBYTES(s), n, dig)) sha256_cpu((uint8_t *)STRBYTES(s), n, dig);
    }
}

/* hashString type s -> lowercase hex (BYTECODE.md). Only "sha256" -- it is
 * what the card hashes, and what store paths need; md5, sha1 and sha512 are
 * E_UNSUP until something needs them, anything else E_TYPE. Nix forces the
 * type first, then the string, which must BE a string (forceString, not a
 * coercion). */
void prim_hashstring(void)
{
    uint8_t dig[32];
    obj_t *t, *r;
    force_slot(2);                              /* the type, first, as Nix forces it */
    t = deref(vstack[vsp - 2]);
    need(t, T_STRING);
    if (CTX(t)) type_err();                     /* forceStringNoCtx */
    if (!is_lit(t, "sha256", 6)) {
        if (is_lit(t, "md5", 3) || is_lit(t, "sha1", 4) || is_lit(t, "sha512", 6)) err_exit(E_UNSUP);
        type_err();
    }
    force_top();
    str_sha256(peek(), dig);
    vsp -= 2;
    r = str_alloc(64);                          /* 72 bytes: inside the boundary's margin */
    hex_of(dig, 32, STRBYTES(r));
    push(r);
}

/* --- string context (docs/DERIVATION.md "String context") --------------------------- */

/* The union of the contexts of the n values at vstack[at..at+n) (anything but
 * a STRING has none): 0; the one list, when a single distinct list is involved
 * -- the usual case, no allocation; else a fresh merged LIST. The merge may
 * collect (every value is on the stack) and turns the ring over between parts,
 * so the caller holds no view across it. */
obj_t *ctx_join(uint16_t at, uint16_t n)
{
    uint16_t i, j, k, m, i2, base = vsp;
    uint8_t c = 0;
    obj_t *x = 0, *l, *s, *e;
    for (i = 0; i < n; ++i) {
        s = deref_follow(vstack[at + i]);
        if (s->tag != T_STRING || !CTX(s) || CTX(s) == x) continue;
        if (x) break;                           /* a second list: merge */
        x = CTX(s);
    }
    if (i == n) return x;
    for (i = 0; i < n; ++i) {
        windows_maybe();
        s = deref_follow(vstack[at + i]);
        if (s->tag != T_STRING || !(l = CTX(s))) continue;
        m = AUX16(deref(l));
        for (j = 0; j < m; ++j) {               /* insertion into vstack[base..vsp), sorted, once */
            e = list_elem(l, j, m);
            for (k = base; k < vsp && (c = (uint8_t)str_cmp(deref(vstack[k]), deref(e))) == 0; ++k) ;
            if (k < vsp && c == 1) continue;
            push(e);
            for (i2 = vsp - 1; i2 > k; --i2) vstack[i2] = vstack[i2 - 1];
            vstack[k] = e;
        }
    }
    return list_from_stack(vsp - base);
}

/* INTERPOLATE n, and a string `+`, which is the same thing with two parts.
 * Each part is forced and coerced in order, as "${}" coerces -- a set through
 * __toString or outPath, which is how "${drv}" becomes its store path -- then
 * they are concatenated, their contexts united. It was run.c's; it is here
 * because bank 0 is full and every string it builds must carry a context. */
void interpolate(uint16_t n)
{
    uint16_t i, len = 0, at = vsp - n;
    obj_t *o, *s;
    for (i = 0; i < n; ++i) {
        force_slot(n - i);
        if (deref(vstack[at + i])->tag != T_STRING) {
            push(vstack[at + i]);
            coerce_top(CO_PLAIN);
            vstack[at + i] = pop();
        }
    }
    push(ctx_join(at, n));                      /* a root while the result is reserved */
    for (i = 0; i < n; ++i) {
        s = deref(vstack[at + i]);
        need(s, T_STRING);
        if ((uint16_t)(len + AUX16(s)) < len) nomem();
        len += AUX16(s);
    }
    nursery_reserve_n(1, round4(len), 8);       /* the parts are on the stack; the copy loop re-derefs */
    o = str_alloc(len);
    len = 0;
    for (i = 0; i < n; ++i) {
        s = deref(vstack[at + i]);
        memcpy(STRBYTES(o) + len, STRBYTES(s), AUX16(s));
        len += AUX16(s);
    }
    STRCTX(o, len) = pop();
    vsp = at;
    push(o);
}

/* --- building contexts -------------------------------------------------------------------
 * An element is a STRING in Nix's syntax ("!<o>!<drv>", "=<drv>", or a plain
 * path), and a context a LIST of them sorted by bytes, no two equal. These
 * make and collect elements; derivation.c's closure and the context builtins
 * below share them. */

/* A fresh STRING: `pre` (when not 0), then "!<o>!" when o (a STRING original)
 * is given, then n bytes of the STRING original s from offset off (CTX_REST:
 * to its end). It reserves first, so it may collect: s and o are on the stack
 * across that. The result is young and not yet rooted; the caller roots it
 * (ctx_insert, push) before anything else allocates. */
obj_t *ctx_piece(char pre, obj_t *o, obj_t *s, uint16_t off, uint16_t n)
{
    uint16_t ol, at;
    obj_t *r;
    push(s);
    push(o);
    ol = o ? AUX16(deref(o)) : 0;
    if (n == CTX_REST) n = AUX16(deref(s)) - off;
    at = (pre ? 1 : 0) + (o ? 2 + ol : 0);
    nursery_reserve(round4(at + n) + 8);
    o = pop();
    s = pop();
    r = str_alloc(at + n);
    if (pre) STRBYTES(r)[0] = pre;
    if (o) {
        STRBYTES(r)[at - 2 - ol] = '!';
        memcpy(STRBYTES(r) + at - 1 - ol, STRBYTES(deref(o)), ol);
        STRBYTES(r)[at - 1] = '!';
    }
    memcpy(STRBYTES(r) + at, STRBYTES(deref(s)) + off, n);
    return r;
}

/* Insert the STRING r into the set vstack[base..vsp), sorted by bytes, once:
 * 1 if it was not there. Allocates nothing. */
int ctx_insert(uint16_t base, obj_t *r)
{
    uint16_t k, i;
    uint8_t c = 0;
    for (k = base; k < vsp && (c = (uint8_t)str_cmp(deref(vstack[k]), deref(r))) == 0; ++k) ;
    if (k < vsp && c == 1) return 0;
    push(r);
    for (i = vsp - 1; i > k; --i) vstack[i] = vstack[i - 1];
    vstack[k] = r;
    return 1;
}

/* A copy of the STRING original so's bytes with the context x (a LIST
 * original, or 0). May collect; both are on the stack across it. Young, not
 * yet rooted. */
static obj_t *str_with_ctx(obj_t *so, obj_t *x)
{
    uint16_t n;
    obj_t *r;
    push(so);
    push(x);
    n = AUX16(deref(so));
    nursery_reserve(round4(n) + 8);
    x = pop();
    so = pop();
    r = str_alloc(n);
    memcpy(STRBYTES(r), STRBYTES(deref(so)), n);
    STRCTX(r, n) = x;
    return r;
}

/* A fresh STRING of n literal bytes (a literal is kept with the code, not in
 * bank 0). May collect. */
static obj_t *lit_str(const char *t, uint16_t n)
{
    obj_t *r;
    nursery_reserve(round4(n) + 8);
    r = str_alloc(n);
    memcpy(STRBYTES(r), t, n);
    return r;
}

/* --- the builtins that need GS/OS (DECISIONS.md D26) ------------------------------------ */

/* Files, the environment, the network and the store are GS/OS's, and the VM
 * does not run under it yet. Such a builtin forces its arguments, first to
 * last, as Nix does before it touches any of them -- so a throw in one is
 * still caught -- and is E_GSOS, its name in RESULT for the harness to print.
 * The names are PRIM_FETCHGIT's onward, in order. */
#define GSOS_NAMES \
    "fetchGit fetchMercurial fetchTarball fetchTree fetchurl filterSource getEnv getFlake hashFile path pathExists readDir readFile readFileType storePath "
_Noreturn void prim_gsos(uint16_t id, uint16_t arity)
{
    const char *t = GSOS_NAMES;
    uint16_t k, n;
    for (k = arity; k; --k) force_slot(k);
    for (k = id - PRIM_FETCHGIT; k; --k) {
        while (*t != ' ') ++t;
        ++t;
    }
    for (n = 0; t[n] != ' '; ++n) ;
    *(obj_t **)RESULT_ADDR = lit_str(t, n);
    err_exit(E_GSOS);
}

/* --- versions and names: Nix's libstore/names.cc, rule by rule ------------------------- */
#define IS_DIGIT(c) ((c) >= '0' && (c) <= '9')
#define IS_ALPHA(c) (((c) | 0x20) >= 'a' && ((c) | 0x20) <= 'z')   /* isalpha, C locale */

/* The forced STRING without context at stack slot k (1 = the top): its view.
 * Nix's forceStringNoCtx. */
static obj_t *nocx_slot(uint16_t k)
{
    obj_t *s;
    force_slot(k);
    s = deref(vstack[vsp - k]);
    if (s->tag != T_STRING || CTX(s)) type_err();
    return s;
}

/* nextComponent: skip '.' and '-', then a run of digits, or a run of
 * anything else but digits, '.' and '-'. Its length (0 at the end); *start
 * is where it begins and *p moves past it. */
static uint16_t next_component(const char *s, uint16_t n, uint16_t *p, uint16_t *start)
{
    while (*p < n && (s[*p] == '.' || s[*p] == '-')) ++*p;
    *start = *p;
    if (*p == n) return 0;
    if (IS_DIGIT(s[*p])) while (*p < n && IS_DIGIT(s[*p])) ++*p;
    else while (*p < n && !IS_DIGIT(s[*p]) && s[*p] != '.' && s[*p] != '-') ++*p;
    return *p - *start;
}

/* string2Int<int>: 1 and *v for a component of digits whose value fits a
 * C++ int, else 0 -- so a longer run of digits compares as text, as in Nix.
 * Unsigned throughout: a signed 32-bit compare links ~GEQL and ~GRTL into
 * bank 0, and v * 10 is shifts for ~UMUL4's sake. */
static int comp_int(const char *c, uint16_t n, uint32_t *v)
{
    uint16_t i;
    *v = 0;
    if (n == 0 || !IS_DIGIT(c[0])) return 0;
    for (i = 0; i < n; ++i) {
        if (*v > 214748364UL || (*v == 214748364UL && c[i] > '7')) return 0;
        *v = (*v << 3) + (*v << 1) + (uint32_t)(c[i] - '0');
    }
    return 1;
}

static int is_pre(const char *c, uint16_t n) { return n == 3 && c[0] == 'p' && c[1] == 'r' && c[2] == 'e'; }

/* componentsLT */
static int comp_lt(const char *a, uint16_t na, const char *b, uint16_t nb)
{
    uint32_t v1, v2;
    int n1 = comp_int(a, na, &v1), n2 = comp_int(b, nb, &v2);
    uint16_t i;
    if (n1 && n2) return v1 < v2;
    if (na == 0 && n2) return 1;
    if (is_pre(a, na) && !is_pre(b, nb)) return 1;
    if (is_pre(b, nb)) return 0;
    if (n2) return 1;                           /* 2.3a < 2.3.1 */
    if (n1) return 0;
    for (i = 0; i < na && i < nb; ++i)          /* string_view's <: bytes, unsigned */
        if (a[i] != b[i]) return (uint8_t)a[i] < (uint8_t)b[i];
    return na < nb;
}

void prim_compareversions(void)          /* v1 v2 -> -1, 0 or 1 */
{
    obj_t *s1, *s2;
    const char *a, *b;
    uint16_t n1, n2, p1 = 0, p2 = 0, c1, c2, l1, l2;
    int r = 0;
    (void)nocx_slot(2);                         /* the first, then the second, as Nix */
    (void)nocx_slot(1);
    s1 = deref(vstack[vsp - 2]);
    s2 = deref(vstack[vsp - 1]);
    a = STRBYTES(s1); n1 = AUX16(s1);
    b = STRBYTES(s2); n2 = AUX16(s2);
    while (p1 < n1 || p2 < n2) {
        l1 = next_component(a, n1, &p1, &c1);
        l2 = next_component(b, n2, &p2, &c2);
        if (comp_lt(a + c1, l1, b + c2, l2)) { r = -1; break; }
        if (comp_lt(b + c2, l2, a + c1, l1)) { r = 1; break; }
    }
    vsp -= 2;
    push(new_int(r));
}

/* A fresh STRING of the n bytes at offset off of the STRING original so; no
 * context. May collect. */
static obj_t *str_piece(obj_t *so, uint16_t off, uint16_t n)
{
    obj_t *r;
    push(so);
    nursery_reserve(round4(n) + 8);
    so = pop();
    r = str_alloc(n);
    memcpy(STRBYTES(r), STRBYTES(deref(so)) + off, n);
    return r;
}

void prim_splitversion(void)             /* v -> [ components ] */
{
    uint16_t b = rsp, p = 0, start, len, cnt = 0;
    obj_t *s = nocx_slot(1);
    rpush(pop());
    for (;;) {
        s = deref(rstack[b]);
        len = next_component(STRBYTES(s), AUX16(s), &p, &start);
        if (len == 0) break;
        push(str_piece(rstack[b], start, len));
        ++cnt;
    }
    rsp = b;
    push(list_from_stack(cnt));
}

/* parseDrvName s -> { name; version; }: the name is everything before the
 * first '-' that is followed by a non-letter, the version everything after
 * it; with no such '-', all of s is the name. */
void prim_parsedrvname(void)
{
    uint16_t b = rsp, i, n, cnt = 0;
    obj_t *s = nocx_slot(1), *set;
    rpush(pop());
    n = AUX16(s);
    for (i = 0; i < n; ++i)
        if (STRBYTES(s)[i] == '-' && i + 1 < n && !IS_ALPHA(STRBYTES(s)[i + 1])) break;
    push(str_piece(rstack[b], 0, i));
    if (i < n) push(str_piece(rstack[b], i + 1, n - i - 1));
    else push(str_piece(rstack[b], n, 0));
    push(lit_str("version", 7));
    nursery_reserve_n(2, 12, 4);
    set = attrs_alloc(2);
    insert_entry(set, 2, &cnt, str_name, vstack[vsp - 3]);
    insert_entry(set, 2, &cnt, vstack[vsp - 1], vstack[vsp - 2]);
    vsp -= 3;
    rsp = b;
    push(set);
}

/* replaceStrings from to s. Nix forces both lists, checks they are as long,
 * forces each `from` (a string; its context is dropped) and then s. At each
 * position the first `from` that matches is replaced by its `to`, forced the
 * first time it is used -- in place on the stack, Nix's cache -- and an empty
 * `from` matches everywhere, before each byte and at the end. The result's
 * context is s's and every used `to`'s. Stack: from[n] to[n] used[n] s. */
static int replace_at(uint16_t at, uint16_t n, uint16_t p)   /* the first from that matches at p, or n */
{
    obj_t *s = deref(vstack[at + 3 * n]), *f;
    uint16_t i, len = AUX16(s);
    for (i = 0; i < n; ++i) {
        f = deref(vstack[at + i]);
        if (len - p >= AUX16(f) && memcmp(STRBYTES(deref(vstack[at + 3 * n])) + p, STRBYTES(f), AUX16(f)) == 0)
            return i;
    }
    return n;
}

void prim_replacestrings(void)
{
    uint16_t b = rsp, n, i, p, at, len, total = 0, used = 0, w;
    obj_t *l, *r, *x, *t;
    force_slot(3);
    if (deref(vstack[vsp - 3])->tag != T_LIST) type_err();
    force_slot(2);
    if (deref(vstack[vsp - 2])->tag != T_LIST) type_err();
    n = AUX16(deref(vstack[vsp - 3]));
    if (AUX16(deref(vstack[vsp - 2])) != n) type_err();
    rpush(vstack[vsp - 3]);                     /* b: from */
    rpush(vstack[vsp - 2]);                     /* b+1: to */
    rpush(vstack[vsp - 1]);                     /* b+2: s */
    vsp -= 3;
    if ((uint16_t)(VSTACK_N - vsp) < 3 * n + 8) nomem();
    at = vsp;
    for (i = 0; i < n; ++i) {
        push(RELEM(b, i, n));
        force_top();
        if (deref(peek())->tag != T_STRING) type_err();
    }
    for (i = 0; i < n; ++i) push(RELEM(b + 1, i, n));
    for (i = 0; i < n; ++i) push(0);
    push(rstack[b + 2]);
    force_top();
    if (deref(peek())->tag != T_STRING) type_err();
    rsp = b;
    /* pass 1, running bytecode: force each `to` at its first use */
    len = AUX16(deref(vstack[at + 3 * n]));
    for (p = 0; p <= len; ) {
        i = replace_at(at, n, p);
        if (i == n) { ++p; continue; }
        if (!vstack[at + 2 * n + i]) {
            force_slot(vsp - (at + n + i));
            if (deref(vstack[at + n + i])->tag != T_STRING) type_err();
            vstack[at + 2 * n + i] = obj_true;
            ++used;
        }
        w = AUX16(deref(vstack[at + i]));
        p += w ? w : 1;
    }
    /* the context: s's and the used `to`s' */
    push(vstack[at + 3 * n]);
    for (i = 0; i < n; ++i) if (vstack[at + 2 * n + i]) push(vstack[at + n + i]);
    x = ctx_join(vsp - used - 1, used + 1);
    vsp -= used + 1;
    push(x);
    /* pass 2, no bytecode: the length; then the bytes */
    for (p = 0; p <= len; ) {
        i = replace_at(at, n, p);
        if (i == n) { total += p < len; ++p; continue; }
        total += AUX16(deref(vstack[at + n + i]));
        w = AUX16(deref(vstack[at + i]));
        if (w) p += w;
        else { total += p < len; ++p; }
    }
    nursery_reserve(round4(total) + 8);
    r = str_alloc(total);
    for (p = 0, total = 0; p <= len; ) {
        i = replace_at(at, n, p);
        if (i == n) {
            if (p < len) STRBYTES(r)[total++] = STRBYTES(deref(vstack[at + 3 * n]))[p];
            ++p;
            continue;
        }
        t = deref(vstack[at + n + i]);
        memcpy(STRBYTES(r) + total, STRBYTES(t), AUX16(t));
        total += AUX16(t);
        w = AUX16(deref(vstack[at + i]));
        if (w) p += w;
        else { if (p < len) STRBYTES(r)[total++] = STRBYTES(deref(vstack[at + 3 * n]))[p]; ++p; }
    }
    l = pop();                                  /* the context */
    STRCTX(r, total) = l;
    vsp = at;
    push(r);
}

/* builtins.toPath s: Nix's coerceToPath -- s coerced as "${}" does, a path
 * as its text (no store copy), which must be absolute, then canonical -- but
 * as a STRING, with s's context (toPath is deprecated, and returns a string). */
void prim_topath(void)
{
    obj_t *s, *p, *r;
    uint16_t n;
    coerce_top(CO_PATH);
    s = deref(peek());
    n = AUX16(s);
    if (n == 0 || STRBYTES(s)[0] != '/') type_err();   /* "doesn't represent an absolute path" */
    nursery_reserve(2 * (round4(n + 1) + 12));          /* the canonical PATH, then the STRING */
    s = deref(peek());
    p = path_join(STRBYTES(s), n, "", 0);
    r = str_alloc(AUX16(p));
    memcpy(STRBYTES(r), STRBYTES(p), AUX16(p));
    STRCTX(r, AUX16(p)) = CTX(deref(peek()));
    vstack[vsp - 1] = r;
}

/* --- the context builtins (DERIVATION.md "String context") -------------------------------- */

/* Where the store path an element (a view) names begins: after the second "!"
 * of "!<o>!<drv>", after "=", or 0 for a plain path. */
static uint16_t elem_path(obj_t *e)
{
    const char *p = STRBYTES(e);
    uint16_t m = AUX16(e), i;
    if (p[0] == '=') return 1;
    if (p[0] != '!') return 0;
    for (i = 1; i < m && p[i] != '!'; ++i) ;
    return i + 1;
}

static int is_drv_name(const char *p, uint16_t n)
{
    return n >= 4 && memcmp(p + n - 4, ".drv", 4) == 0;
}

/* Nix's isStorePath: "/nix/store/", 32 nix32 digits, "-", and a store-path
 * name (check_name, which is E_TYPE itself). */
static int is_store_path(const char *p, uint16_t n)
{
    uint16_t i, d;
    if (n < 45 || memcmp(p, "/nix/store/", 11) != 0 || p[43] != '-') return 0;
    for (i = 11; i < 43; ++i) {
        for (d = 0; d < 32 && "0123456789abcdfghijklmnpqrsvwxyz"[d] != p[i]; ++d) ;
        if (d == 32) return 0;
    }
    check_name(p + 44, n - 44);
    return 1;
}

/* hasContext s: s must BE a string (forceString, not a coercion). */
void prim_hascontext(void)
{
    obj_t *so, *s = force_pop_as(T_STRING, &so);
    push(CTX(s) ? obj_true : obj_false);
}

/* unsafeDiscardStringContext s: s coerced as "${}" coerces it (a set through
 * __toString or outPath), and returned without its context. */
void prim_unsafediscardstringcontext(void)
{
    coerce_top(CO_PLAIN);
    if (CTX(deref(peek()))) vstack[vsp - 1] = str_with_ctx(peek(), 0);
}

/* unsafeDiscardOutputDependency s: coerced; each "=<drv>" becomes the plain
 * path <drv> -- an opaque source, which derivationStrict does not expand. */
void prim_unsafediscardoutputdependency(void)
{
    uint16_t n, i, base;
    obj_t *x, *e;
    coerce_top(CO_PLAIN);
    x = CTX(deref(peek()));
    if (!x) return;
    n = AUX16(deref(x));
    for (i = 0; i < n && STRBYTES(deref(list_elem(x, i, n)))[0] != '='; ++i) ;
    if (i == n) return;
    push(x);                                    /* base - 1: the old context */
    base = vsp;
    for (i = 0; i < n; ++i) {
        windows_maybe();
        e = list_elem(vstack[base - 1], i, n);
        if (STRBYTES(deref(e))[0] == '=') ctx_insert(base, ctx_piece(0, 0, e, 1, CTX_REST));
        else ctx_insert(base, e);
    }
    x = list_from_stack(vsp - base);
    --vsp;                                      /* the coerced string is on top again */
    vstack[vsp - 1] = str_with_ctx(peek(), x);
}

/* addDrvOutputDependencies s: coerced; its context must be one element: a
 * plain .drv path, which becomes "=<drv>", or "=<drv>" already. An output, or
 * a path that is not a .drv, is an error. */
void prim_adddrvoutputdependencies(void)
{
    obj_t *x, *e;
    const char *p;
    uint16_t m;
    coerce_top(CO_PLAIN);
    x = CTX(deref(peek()));
    if (!x || AUX16(deref(x)) != 1) type_err();         /* "must have exactly one element" */
    e = list_elem(x, 0, 1);
    p = STRBYTES(deref(e));
    m = AUX16(deref(e));
    if (p[0] == '=') return;
    if (p[0] == '!' || !is_drv_name(p, m)) type_err();
    push(ctx_piece('=', 0, e, 0, CTX_REST));
    x = list_from_stack(1);
    vstack[vsp - 1] = str_with_ctx(peek(), x);
}

/* getContext s: s a string. { "<path>" = { allOutputs = true; outputs = [
 * "<o>" ... ]; path = true; }; ... }: one attribute per store path the context
 * names; of its three, only those that are true or not empty (Nix's
 * ContextInfo); its outputs in name order, which is the elements' own. */
void prim_getcontext(void)
{
    uint16_t n, i, j, k, nk, kb, ob, ko, cnt;
    uint8_t all, path;
    obj_t *so, *s, *e, *ev, *kv, *r;
    s = force_pop_as(T_STRING, &so);
    if (!CTX(s)) { push(attrs_alloc(0)); return; }     /* 8 bytes: inside the boundary's margin */
    push(CTX(s));                               /* kb - 3: the context */
    push(lit_str("allOutputs", 10));            /* kb - 2 */
    push(lit_str("outputs", 7));                /* kb - 1 */
    kb = vsp;                                   /* the keys: each element's path, sorted, once */
    n = AUX16(deref(vstack[kb - 3]));
    for (i = 0; i < n; ++i) {
        windows_maybe();
        e = list_elem(vstack[kb - 3], i, n);
        ctx_insert(kb, ctx_piece(0, 0, e, elem_path(deref(e)), CTX_REST));
    }
    nk = vsp - kb;
    for (k = 0; k < nk; ++k) {                  /* each key's value, after the keys */
        all = path = 0;
        ob = vsp;                               /* its outputs' names */
        for (i = 0; i < n; ++i) {
            windows_maybe();
            e = list_elem(vstack[kb - 3], i, n);
            ev = deref(e);
            kv = deref(vstack[kb + k]);
            ko = elem_path(ev);
            if (AUX16(ev) - ko != AUX16(kv) || memcmp(STRBYTES(ev) + ko, STRBYTES(kv), AUX16(kv)) != 0)
                continue;
            if (STRBYTES(ev)[0] == '=') all = 1;
            else if (STRBYTES(ev)[0] != '!') path = 1;
            else push(ctx_piece(0, 0, e, 1, ko - 2));
        }
        cnt = all + path + (vsp > ob);
        if (vsp > ob) {
            r = list_from_stack(vsp - ob);
            push(r);
        }
        nursery_reserve(4 + 12 * cnt);
        r = attrs_alloc(cnt);
        j = 0;
        if (all) attrs_put(r, cnt, j++, vstack[kb - 2], obj_true);
        if (vsp > ob) attrs_put(r, cnt, j++, vstack[kb - 1], vstack[vsp - 1]);
        if (path) attrs_put(r, cnt, j++, str_path, obj_true);
        if (vsp > ob) --vsp;
        push(r);
    }
    nursery_reserve(4 + 12 * nk);
    r = attrs_alloc(nk);
    for (k = 0; k < nk; ++k) attrs_put(r, nk, k, vstack[kb + k], vstack[kb + nk + k]);
    vsp = kb - 3;
    push(r);
}

/* appendContext s ctx: s, its own context kept, with for each store path in
 * ctx a plain path (path = true), "=<drv>" (allOutputs = true) and
 * "!<o>!<drv>" (each of outputs) -- the last two for a .drv only. A key that is
 * not a store path is an error. A read-only Nix does not ask its store
 * whether the path exists, and the VM has no store to ask. */
#define AC_KEY() ANAMES(deref(vstack[b0 - 1]), n)[i]
void prim_appendcontext(void)
{
    uint16_t n, i, j, m, b0, base, ix;
    obj_t *s, *v, *x, *f, *nmv;
    force_slot(2);                              /* s first, as Nix forces it */
    s = deref(vstack[vsp - 2]);
    need(s, T_STRING);
    force_top();
    need(deref(peek()), T_ATTRS);
    b0 = vsp;                                   /* b0 - 2: s, b0 - 1: ctx */
    push(lit_str("allOutputs", 10));            /* b0 */
    push(lit_str("outputs", 7));                /* b0 + 1 */
    base = vsp;                                 /* the added elements */
    n = AUX16(deref(vstack[b0 - 1]));
    for (i = 0; i < n; ++i) {
        windows_maybe();
        nmv = deref(AC_KEY());
        if (!is_store_path(STRBYTES(nmv), AUX16(nmv))) type_err();   /* "is not a store path" */
        push(AVALUES(deref(vstack[b0 - 1]), n)[i]);
        (void)force_pop_as(T_ATTRS, &x);
        rpush(x);                               /* the key's value */
        if (find_attr(rstack[rsp - 1], str_path, &ix, &f)) {
            push(f);
            if (BOOLVAL(force_pop_as(T_BOOL, &x))) ctx_insert(base, ctx_piece(0, 0, AC_KEY(), 0, CTX_REST));
        }
        if (find_attr(rstack[rsp - 1], vstack[b0], &ix, &f)) {
            push(f);
            if (BOOLVAL(force_pop_as(T_BOOL, &x))) {
                nmv = deref(AC_KEY());
                if (!is_drv_name(STRBYTES(nmv), AUX16(nmv))) type_err();
                ctx_insert(base, ctx_piece('=', 0, AC_KEY(), 0, CTX_REST));
            }
        }
        if (find_attr(rstack[rsp - 1], vstack[b0 + 1], &ix, &f)) {
            push(f);
            v = force_pop_as(T_LIST, &x);       /* not AUX16(force_pop_as(...)): AUX16 reads its argument twice */
            m = AUX16(v);
            nmv = deref(AC_KEY());
            if (m && !is_drv_name(STRBYTES(nmv), AUX16(nmv))) type_err();
            rpush(x);                           /* the outputs list */
            for (j = 0; j < m; ++j) {
                push(RELEM(rsp - 1, j, m));
                v = force_pop_as(T_STRING, &x);
                if (CTX(v)) type_err();         /* forceStringNoCtx */
                ctx_insert(base, ctx_piece(0, x, AC_KEY(), 0, CTX_REST));
            }
            rpop();
        }
        rpop();
    }
    x = CTX(deref(vstack[b0 - 2]));             /* s's own context joins them */
    if (x) {
        rpush(x);
        m = AUX16(deref(x));
        for (j = 0; j < m; ++j) {
            windows_maybe();
            ctx_insert(base, RELEM(rsp - 1, j, m));
        }
        rpop();
    }
    x = vsp > base ? list_from_stack(vsp - base) : 0;
    vsp = b0 - 1;                               /* s is on top */
    if (x) vstack[vsp - 1] = str_with_ctx(peek(), x);
}

/* concatStringsSep sep list, with the result left on the stack. */
static void concat_strings_sep_top(void)
{
    uint16_t b = rsp, n, i, total = 0, nsep, w = 0, at = 0;
    uint8_t coerce = 0, hasctx = 0;
    obj_t *sep, *l, *sepo, *lo, *e, *r;
    force_two_orig(&sep, &l, &sepo, &lo);
    need(sep, T_STRING); need(l, T_LIST);
    n = AUX16(l);
    rpush(sepo);
    rpush(lo);
    /* Force every element and measure it, stopping at the first that is not a
     * string. One pass, not a separate measuring one: with every element
     * mature, each pass copies each string into the window ring, and three
     * passes over 1,000 strings overran an instruction's 44 KB of ring
     * (eval-okay-big-operations under A2MIN_RESERVE_STRESS). */
    for (i = 0; i < n; ++i) {
        push(RELEM(b + 1, i, n));
        force_top();
        e = deref(pop());
        if (e->tag != T_STRING) { coerce = 1; break; }
        if (CTX(e)) hasctx = 1;
        if ((uint16_t)(total + AUX16(e)) < total) nomem();
        total += AUX16(e);
    }
    if (!coerce && (hasctx || CTX(deref(rstack[b])))) {
        /* A context to unite: the elements go on the stack, as coerced ones
         * would, for ctx_join to see and the copy to read. */
        at = vsp;
        for (i = 0; i < n; ++i) push(RELEM(b + 1, i, n));
        coerce = 2;
    } else if (coerce) {
        total = 0;
        /* Some element coerces (a set via __toString / outPath), so the text
         * to copy is the COERCED value, not the element: keep every coerced
         * string on the value stack, a root, and copy from there. (The first
         * version coerced to measure and then copied the original elements --
         * a set's header bytes as text: "F\x9a,\x9a,plain".) Bounded by the
         * value stack; loud E_NOMEM past it. */
        at = vsp;
        for (i = 0; i < n; ++i) {
            push(RELEM(b + 1, i, n));
            coerce_top(CO_PLAIN);
            e = deref(peek());
            if ((uint16_t)(total + AUX16(e)) < total) nomem();
            total += AUX16(e);
        }
    }
    sepo = rstack[b];
    sep = deref(sepo);
    nsep = AUX16(sep);
    for (i = 1; i < n; ++i) {                   /* n-1 separators */
        if ((uint16_t)(total + nsep) < total) nomem();
        total += nsep;
    }
    if (coerce) {                               /* the elements' contexts and the separator's */
        push(rstack[b]);
        e = ctx_join(at, n + 1);
        --vsp;
    } else {
        e = 0;
    }
    push(e);                                    /* a root across the reserve */
    nursery_reserve_n(1, round4(total), 8);     /* sep, the list, any coerced strings: all rooted */
    sepo = rstack[b];
    lo = rstack[b + 1];
    rsp = b;
    r = str_alloc(total);
    for (i = 0; i < n; ++i) {
        if (i) { sep = deref(sepo); memcpy(STRBYTES(r) + w, STRBYTES(sep), nsep); w += nsep; }
        if (coerce) e = deref_follow(vstack[at + i]);
        else e = deref_follow(list_elem(lo, i, n));   /* not deref(lo): a long mature list re-copied per element overran the window ring */
        memcpy(STRBYTES(r) + w, STRBYTES(e), AUX16(e));
        w += AUX16(e);
    }
    STRCTX(r, total) = pop();
    if (coerce) vsp = at;
    push(r);
}

void prim_concatstringssep(void) { concat_strings_sep_top(); }

/* baseNameOf / dirOf on STRINGs, with Nix's edge cases. */
void prim_basenameof(void)
{
    obj_t *s = view_top_text(), *r;       /* stays rooted until the reserve */
    uint16_t n = AUX16(s), last, start;
    const char *p = STRBYTES(s);
    if (n == 0) {
        r = str_alloc(0);
        if (s->tag == T_STRING) STRCTX(r, 0) = CTX(s);
        --vsp;
        push(r);
        return;
    }
    last = n - 1;
    if (last && p[last] == '/') --last;        /* a trailing slash is ignored */
    start = last;
    for (;;) {
        if (p[start] == '/') { ++start; break; }
        if (start == 0) break;
        --start;
    }
    if (nursery_reserve_n(1, round4(last - start + 1), 8)) { s = view_top_text(); p = STRBYTES(s); }
    --vsp;
    r = str_alloc(last - start + 1);
    memcpy(STRBYTES(r), p + start, last - start + 1);
    if (s->tag == T_STRING) STRCTX(r, last - start + 1) = CTX(s);   /* a string's context stays */
    push(r);
}

/* The static STRING st, or with context x a young copy of it that has it. */
static void push_text(obj_t *st, obj_t *x)
{
    obj_t *v, *r;
    if (!x) { push(st); return; }
    v = deref(st);
    r = str_alloc(AUX16(v));                    /* a byte or two: inside the margin */
    memcpy(STRBYTES(r), STRBYTES(v), AUX16(v));
    STRCTX(r, AUX16(v)) = x;
    push(r);
}

void prim_dirof(void)
{
    obj_t *s = view_top_text(), *r, *x;   /* stays rooted until the reserve */
    uint16_t n = AUX16(s), j;
    const char *p = STRBYTES(s);
    x = s->tag == T_STRING ? CTX(s) : 0;  /* a string's context stays; a path has none */
    if (n == 0) { --vsp; push_text(str_dot, x); return; }
    j = n - 1;
    for (;;) {
        if (p[j] == '/') break;
        if (j == 0) { --vsp; push_text(str_dot, x); return; }
        --j;
    }
    /* A path's directory is a path; a string's is a string. A path always has
     * a slash (it is absolute and canonical), so it never reaches str_dot. */
    if (j == 0) {
        --vsp;
        if (s->tag == T_PATH) { r = path_alloc(1); STRBYTES(r)[0] = '/'; push(r); }
        else push_text(str_slash, x);
        return;
    }
    if (nursery_reserve_n(1, round4(j), 8)) {
        s = view_top_text();
        p = STRBYTES(s);
        x = s->tag == T_STRING ? CTX(s) : 0;    /* the collection may have moved it */
    }
    --vsp;
    r = s->tag == T_PATH ? path_alloc(j) : str_alloc(j);
    memcpy(STRBYTES(r), p, j);
    if (x) STRCTX(r, j) = x;
    push(r);
}
