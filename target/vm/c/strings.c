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

/* hashString type s -> lowercase hex (BYTECODE.md). Only "sha256" -- it is
 * what the card hashes, and what store paths need; md5, sha1 and sha512 are
 * E_UNSUP until something needs them, anything else E_TYPE. Nix forces the
 * type first, then the string, which must BE a string (forceString, not a coercion).
 *
 * The string is hashed where it lives. A mature one stays on the card and
 * HASH_STREAM reads it there: only its 4-byte header crosses the bus. A young
 * one goes through HASH_PUSH. With no card, or the verb masked
 * (checks.verb-economics), sha256.c does it on the CPU. */
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

void prim_hashstring(void)
{
    uint8_t dig[32];
    uint16_t n;
    obj_t *t, *so, *s, *r, hdr;
    force_slot(2);                              /* the type, first, as Nix forces it */
    t = deref(vstack[vsp - 2]);
    need(t, T_STRING);
    if (!is_lit(t, "sha256", 6)) {
        if (is_lit(t, "md5", 3) || is_lit(t, "sha1", 4) || is_lit(t, "sha512", 6)) err_exit(E_UNSUP);
        type_err();
    }
    force_top();
    so = peek();
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
