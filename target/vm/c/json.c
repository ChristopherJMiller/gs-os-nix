/* json.c -- builtins.toJSON: Nix's value-to-json.cc, then nlohmann/json's
 * dump (docs/BYTECODE.md "toJSON").
 *
 * Two phases, as Nix has them. The tree: every value forced in the order Nix
 * forces it -- a list's elements in order, a set's values in name order -- a
 * set with __toString coerced through it, one with outPath replaced by its
 * outPath, the strings' contexts united; this runs bytecode. Then the text,
 * with no bytecode and no collection: once to check and count, then the one
 * allocation, then once to write. nlohmann checks UTF-8 when it dumps the
 * finished tree, so an evaluation error anywhere in the value comes before an
 * invalid string, and so it is here.
 *
 * The __toString strings are the one thing the text cannot recompute: the tree
 * leaves them on the value stack in the order it meets them, and the text,
 * which meets the same sets in the same order, takes them from there.
 */
#pragma noroot
#pragma optimize 15

#include "vm.h"
#include "prims.h"

/* With float.c's: "prims" fills bank 4 (VM.md). */
segment "json";

/* --- the tree --------------------------------------------------------------------------- */
/* rstack[acc], a STRING, carries the union of the contexts met so far; s, a
 * STRING original, joins it. May collect: both are on the stacks. */
static void unite(uint16_t acc, obj_t *s)
{
    obj_t *x, *e;
    if (CTX(deref(s)) == 0 || CTX(deref(s)) == CTX(deref(rstack[acc]))) return;
    push(rstack[acc]);
    push(s);
    x = ctx_join(vsp - 2, 2);
    vsp -= 2;
    push(x);
    nursery_reserve(8);
    x = pop();
    e = str_alloc(0);
    STRCTX(e, 0) = x;
    rstack[acc] = e;
}

/* rstack[k], forced, and everything in it: Nix's printValueAsJSON before the
 * dump. */
static void tree(uint16_t k, uint16_t acc)
{
    obj_t *v, *x;
    uint16_t n, i, ix;
    stack_check();
    push(rstack[k]);
    force_top();
    rstack[k] = pop();
    v = deref(rstack[k]);
    switch (v->tag) {
    case T_STRING:
        unite(acc, rstack[k]);
        break;
    case T_PATH:                                /* Nix copies it to the store (L7) */
        err_exit(E_UNSUP);
    case T_LAMBDA:                              /* "cannot convert a function to JSON" */
        type_err();
    case T_LIST:
        n = AUX16(v);
        rpush(0);
        for (i = 0; i < n; ++i) {
            rstack[rsp - 1] = RELEM(k, i, n);
            tree(rsp - 1, acc);
        }
        --rsp;
        break;
    case T_ATTRS:
        if (find_attr(rstack[k], str_tostring, &ix, &x)) {
            push(rstack[k]);                    /* tryAttrsToString: no store copy */
            coerce_top(CO_PATH);
            unite(acc, peek());                 /* and it stays, for the text */
        } else if (find_attr(rstack[k], str_outpath, &ix, &x)) {
            rstack[k] = x;
            tree(k, acc);
        } else {
            n = AUX16(v);
            rpush(0);
            for (i = 0; i < n; ++i) {
                rstack[rsp - 1] = AVALUES(deref(rstack[k]), n)[i];
                tree(rsp - 1, acc);
            }
            --rsp;
        }
        break;
    default:                                    /* INT, FLOAT, BOOL, NULL */
        break;
    }
}

/* --- the text --------------------------------------------------------------------------- */
struct text {
    char *buf;                                  /* 0 while counting */
    uint16_t len;                               /* bytes so far */
    uint16_t next;                              /* the next __toString string's stack slot */
};

static void put(struct text *t, const char *s, uint16_t n)
{
    if ((uint16_t)(t->len + n) < t->len) nomem();   /* past 64 KB */
    if (t->buf) memcpy(t->buf + t->len, s, n);
    t->len += n;
}

/* The bytes of a STRING view, as nlohmann's dump_escaped writes them with
 * ensure_ascii off: valid UTF-8 (RFC 3629, what its decoder accepts) passes
 * through; " \ \b \f \n \r \t are escaped, any other byte below 0x20 is
 * \u00xx in lowercase hex; anything that is not UTF-8 is its type_error 316,
 * here E_TYPE. */
static void esc(struct text *t, obj_t *s)
{
    const uint8_t *b = (const uint8_t *)STRBYTES(s);
    uint16_t n = AUX16(s), i = 0, j, more;
    uint8_t c, lo, hi;
    char e[6];
    put(t, "\"", 1);
    while (i < n) {
        c = b[i];
        if (c < 0x80) {
            if (c == '"' || c == '\\') { e[0] = '\\'; e[1] = (char)c; put(t, e, 2); }
            else if (c == '\b') put(t, "\\b", 2);
            else if (c == '\f') put(t, "\\f", 2);
            else if (c == '\n') put(t, "\\n", 2);
            else if (c == '\r') put(t, "\\r", 2);
            else if (c == '\t') put(t, "\\t", 2);
            else if (c < 0x20) {
                e[0] = '\\'; e[1] = 'u'; e[2] = '0'; e[3] = '0';
                e[4] = (char)('0' + (c >> 4));
                e[5] = (char)((c & 15) < 10 ? '0' + (c & 15) : 'a' + (c & 15) - 10);
                put(t, e, 6);
            } else put(t, (const char *)b + i, 1);
            ++i;
            continue;
        }
        /* the lead byte's sequence length and the second byte's range */
        lo = 0x80; hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) more = 1;
        else if (c >= 0xE0 && c <= 0xEF) {
            more = 2;
            if (c == 0xE0) lo = 0xA0;           /* no overlong form */
            if (c == 0xED) hi = 0x9F;           /* no surrogate */
        } else if (c >= 0xF0 && c <= 0xF4) {
            more = 3;
            if (c == 0xF0) lo = 0x90;
            if (c == 0xF4) hi = 0x8F;           /* nothing past U+10FFFF */
        } else type_err();
        if ((uint16_t)(n - i) <= more) type_err();   /* an incomplete sequence */
        if (b[i + 1] < lo || b[i + 1] > hi) type_err();
        for (j = 2; j <= more; ++j)
            if (b[i + j] < 0x80 || b[i + j] > 0xBF) type_err();
        put(t, (const char *)b + i, more + 1);
        i += more + 1;
    }
    put(t, "\"", 1);
}

/* An INT's decimal, as int_to_str makes it (strings.c), into buf: its length. */
static uint16_t int_text(int64_t v, char *out)
{
    char buf[24];
    uint16_t n = 0, i;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
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
    for (i = 0; i < n; ++i) out[i] = buf[n - 1 - i];
    return n;
}

/* The value o (an original) as nlohmann dumps it. No bytecode runs and
 * nothing is allocated; the window ring may turn over between values, so a
 * container is viewed again after each of its elements. */
static void text(struct text *t, obj_t *o)
{
    obj_t *v, *x;
    uint16_t n, i, ix;
    uint64_t bits;
    char num[F_JSON_MAX];
    stack_check();
    windows_maybe();
    /* A forced thunk may be an INDIRECT: list_elem and find_attr want the
     * target's original, not the thunk's. */
    for (v = deref(o); v->tag == T_INDIRECT; v = deref(o)) o = IND_TARGET(v);
    switch (v->tag) {
    case T_NULL:   put(t, "null", 4); break;
    case T_BOOL:   if (BOOLVAL(v)) put(t, "true", 4); else put(t, "false", 5); break;
    case T_INT:    put(t, num, int_text(INTVAL(v), num)); break;
    case T_FLOAT:
        memcpy(&bits, PAYLOAD(v), 8);
        put(t, num, f_json(bits, num));
        break;
    case T_STRING: esc(t, v); break;
    case T_LIST:
        n = AUX16(v);
        put(t, "[", 1);
        for (i = 0; i < n; ++i) {
            if (i) put(t, ",", 1);
            text(t, list_elem(o, i, n));
        }
        put(t, "]", 1);
        break;
    case T_ATTRS:
        if (find_attr(o, str_tostring, &ix, &x)) {
            esc(t, deref(vstack[t->next++]));   /* the tree's __toString string */
        } else if (find_attr(o, str_outpath, &ix, &x)) {
            text(t, x);
        } else {
            n = AUX16(v);
            put(t, "{", 1);
            for (i = 0; i < n; ++i) {
                if (i) put(t, ",", 1);
                v = deref(o);
                esc(t, deref(ANAMES(v, n)[i]));
                put(t, ":", 1);
                text(t, AVALUES(deref(o), n)[i]);
            }
            put(t, "}", 1);
        }
        break;
    default:
        badnbc();                               /* the tree lets nothing else through: a VM bug */
    }
}

/* builtins.toJSON v: the tree, the text counted, one STRING, the text written,
 * with every string's context. */
void prim_tojson(void)
{
    uint16_t b = rsp, at;
    struct text t;
    obj_t *r;
    rpush(pop());                               /* b: the value */
    nursery_reserve(8);
    rpush(str_alloc(0));                        /* b+1: the context so far */
    at = vsp;
    tree(b, b + 1);
    t.buf = 0; t.len = 0; t.next = at;
    text(&t, rstack[b]);
    nursery_reserve(round4(t.len) + 8);         /* the stacks hold everything */
    r = str_alloc(t.len);
    t.buf = STRBYTES(r); t.len = 0; t.next = at;
    text(&t, rstack[b]);
    STRCTX(r, t.len) = CTX(deref(rstack[b + 1]));
    rsp = b;
    vsp = at;
    push(r);
}
