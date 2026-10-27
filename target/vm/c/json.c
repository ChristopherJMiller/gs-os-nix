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
#include "toml.h"

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
        stack_need(FLOAT_STACK);
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

/* --- fromJSON: nlohmann/json's lexer and parser, into Nix's JSONSax --------------------- */
/* builtins.fromJSON s, s a string without context, as json-to-value.cc reads
 * it through nlohmann's sax_parse: a BOM at the start skipped; whitespace the
 * four JSON ones; numbers by JSON's grammar, an integer token through
 * strtoull or strtoll -- one too wide is a float, but an unsigned one above
 * INT64_MAX is Nix's error -- and a float through strtod (float.c f_strtod),
 * an overflow being nlohmann's error; strings with their escapes, surrogate
 * pairs joined, raw bytes held to UTF-8, a NUL Nix's error (forceNoNullByte);
 * an object's last value for a repeated key (insert_or_assign). Every error
 * is E_TYPE. The text is at rstack[k], read through a view that is taken
 * again after anything that allocates. */
struct src {
    uint16_t k, p, n;
};

static const uint8_t *src_at(struct src *s)
{
    return (const uint8_t *)STRBYTES(deref(rstack[s->k]));
}

static void ws(struct src *s)
{
    const uint8_t *b = src_at(s);
    while (s->p < s->n && (b[s->p] == ' ' || b[s->p] == '\t' || b[s->p] == '\n' || b[s->p] == '\r')) ++s->p;
}

static void jlit(struct src *s, const char *lit, uint16_t n)
{
    const uint8_t *b = src_at(s);
    uint16_t i;
    if ((uint16_t)(s->n - s->p) < n) type_err();
    for (i = 0; i < n; ++i) if (b[s->p + i] != (uint8_t)lit[i]) type_err();
    s->p += n;
}

static uint16_t hex4(const uint8_t *b, uint16_t at)
{
    uint16_t v = 0, i;
    uint8_t c;
    for (i = 0; i < 4; ++i) {
        c = b[at + i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') v |= (c | 0x20) - 'a' + 10;
        else type_err();
    }
    return v;
}

/* The string at s->p (its opening quote), decoded into out (0: only
 * measured): its length. *end is past the closing quote. */
static uint16_t jstr(struct src *s, char *out, uint16_t *end)
{
    const uint8_t *b = src_at(s);
    uint16_t p = s->p + 1, n = s->n, len = 0, j, more;
    uint32_t cp;
    uint8_t c, lo, hi;
    for (;;) {
        if (p >= n) type_err();                 /* no closing quote */
        c = b[p];
        if (c == '"') break;
        if (c < 0x20) type_err();               /* a control character must be escaped */
        if (c == '\\') {
            if (p + 1 >= n) type_err();
            c = b[++p];
            ++p;
            if (c == 'u') {
                if ((uint16_t)(n - p) < 4) type_err();
                cp = hex4(b, p);
                p += 4;
                if (cp >= 0xDC00 && cp <= 0xDFFF) type_err();          /* a lone low surrogate */
                if (cp >= 0xD800 && cp <= 0xDBFF) {                    /* a high one: its low must follow */
                    if ((uint16_t)(n - p) < 6 || b[p] != '\\' || b[p + 1] != 'u') type_err();
                    j = hex4(b, p + 2);
                    if (j < 0xDC00 || j > 0xDFFF) type_err();
                    cp = 0x10000UL + ((cp - 0xD800) << 10) + (j - 0xDC00);
                    p += 6;
                }
                if (cp == 0) type_err();        /* Nix strings hold no NUL */
                if (cp < 0x80) {
                    if (out) out[len] = (char)cp;
                    len += 1;
                } else if (cp < 0x800) {
                    if (out) { out[len] = (char)(0xC0 | (cp >> 6)); out[len + 1] = (char)(0x80 | (cp & 0x3F)); }
                    len += 2;
                } else if (cp < 0x10000UL) {
                    if (out) {
                        out[len] = (char)(0xE0 | (cp >> 12));
                        out[len + 1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[len + 2] = (char)(0x80 | (cp & 0x3F));
                    }
                    len += 3;
                } else {
                    if (out) {
                        out[len] = (char)(0xF0 | (cp >> 18));
                        out[len + 1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                        out[len + 2] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[len + 3] = (char)(0x80 | (cp & 0x3F));
                    }
                    len += 4;
                }
                continue;
            }
            switch (c) {
            case '"': case '\\': case '/': break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            default: type_err();
            }
            if (out) out[len] = (char)c;
            ++len;
            continue;
        }
        if (c < 0x80) {
            if (out) out[len] = (char)c;
            ++len;
            ++p;
            continue;
        }
        lo = 0x80; hi = 0xBF;                   /* UTF-8, as toJSON's esc checks it */
        if (c >= 0xC2 && c <= 0xDF) more = 1;
        else if (c >= 0xE0 && c <= 0xEF) { more = 2; if (c == 0xE0) lo = 0xA0; if (c == 0xED) hi = 0x9F; }
        else if (c >= 0xF0 && c <= 0xF4) { more = 3; if (c == 0xF0) lo = 0x90; if (c == 0xF4) hi = 0x8F; }
        else type_err();
        if ((uint16_t)(n - p) <= more) type_err();
        if (b[p + 1] < lo || b[p + 1] > hi) type_err();
        for (j = 2; j <= more; ++j) if (b[p + j] < 0x80 || b[p + j] > 0xBF) type_err();
        for (j = 0; j <= more; ++j) { if (out) out[len] = (char)b[p + j]; ++len; }
        p += more + 1;
    }
    *end = p + 1;
    return len;
}

static void jstring(struct src *s)
{
    uint16_t end, n = jstr(s, 0, &end);
    obj_t *r;
    nursery_reserve(round4(n) + 8);
    r = str_alloc(n);
    (void)jstr(s, STRBYTES(r), &end);
    s->p = end;
    push(r);
}

/* A number: its token by JSON's grammar, then nlohmann's types. */
static void jnumber(struct src *s)
{
    const uint8_t *b = src_at(s);
    uint16_t start = s->p, p = s->p, n = s->n, neg = 0, isint = 1, d;
    uint64_t v = 0, bits;
    int over = 0;
    obj_t *o;
    if (b[p] == '-') { neg = 1; ++p; }
    if (p >= n || b[p] < '0' || b[p] > '9') type_err();
    if (b[p] == '0') ++p;
    else
        for (; p < n && b[p] >= '0' && b[p] <= '9'; ++p) {
            d = b[p] - '0';
            if (v > 1844674407370955161ULL || (v == 1844674407370955161ULL && d > 5)) over = 1;
            else v = v * 10 + d;
        }
    if (p < n && b[p] == '.') {
        isint = 0;
        if (++p >= n || b[p] < '0' || b[p] > '9') type_err();
        while (p < n && b[p] >= '0' && b[p] <= '9') ++p;
    }
    if (p < n && (b[p] == 'e' || b[p] == 'E')) {
        isint = 0;
        if (++p < n && (b[p] == '+' || b[p] == '-')) ++p;
        if (p >= n || b[p] < '0' || b[p] > '9') type_err();
        while (p < n && b[p] >= '0' && b[p] <= '9') ++p;
    }
    s->p = p;
    if (isint && !over && neg && v <= ((uint64_t)1 << 63)) {   /* strtoll: it fits */
        nursery_reserve(12);
        push(new_int((int64_t)((uint64_t)0 - v)));
        return;
    }
    if (isint && !over && !neg) {               /* strtoull: it fits */
        if (v > 0x7FFFFFFFFFFFFFFFULL) type_err();   /* "outside of Nix integer range" */
        nursery_reserve(12);
        push(new_int((int64_t)v));
        return;
    }
    stack_need(FLOAT_STACK);
    bits = f_strtod((const char *)b + start, p - start);
    if (((bits >> 52) & 0x7FF) == 0x7FF) type_err();   /* nlohmann: "number overflow" */
    nursery_reserve(12);
    o = new_float(bits);
    push(o);
}

static void jvalue(struct src *s);

static void jarray(struct src *s)
{
    uint16_t cnt = 0;
    ++s->p;
    ws(s);
    if (s->p < s->n && src_at(s)[s->p] == ']') { ++s->p; push(list_from_stack(0)); return; }
    for (;;) {
        jvalue(s);
        ++cnt;
        ws(s);
        if (s->p >= s->n) type_err();
        if (src_at(s)[s->p] == ',') { ++s->p; continue; }
        if (src_at(s)[s->p] == ']') { ++s->p; break; }
        type_err();
    }
    push(list_from_stack(cnt));
}

static void jobject(struct src *s)
{
    uint16_t cnt = 0, at = vsp, i, got = 0;
    obj_t *set, *r;
    ++s->p;
    ws(s);
    if (s->p < s->n && src_at(s)[s->p] == '}') ++s->p;
    else
        for (;;) {
            ws(s);
            if (s->p >= s->n || src_at(s)[s->p] != '"') type_err();
            jstring(s);                         /* the key: a NUL is already refused */
            ws(s);
            if (s->p >= s->n || src_at(s)[s->p] != ':') type_err();
            ++s->p;
            jvalue(s);
            ++cnt;
            ws(s);
            if (s->p >= s->n) type_err();
            if (src_at(s)[s->p] == ',') { ++s->p; continue; }
            if (src_at(s)[s->p] == '}') { ++s->p; break; }
            type_err();
        }
    /* the last value of a repeated key: insert_entry keeps a name's first,
     * so the pairs go in from the last */
    nursery_reserve_n(cnt, 2 * ATTR_RESERVE, 8);    /* the set and its compacted copy; the pairs are on the stack */
    set = attrs_alloc(cnt);
    for (i = cnt; i-- > 0; ) insert_entry(set, cnt, &got, vstack[at + 2 * i], vstack[at + 2 * i + 1]);
    if (got != cnt) {
        r = attrs_alloc(got);
        for (i = 0; i < got; ++i) copy_entry(r, got, i, set, cnt, i);
        set = r;
    }
    vsp = at;
    push(set);
}

static void jvalue(struct src *s)
{
    uint8_t c;
    stack_check();
    ws(s);
    if (s->p >= s->n) type_err();
    c = src_at(s)[s->p];
    if (c == '{') jobject(s);
    else if (c == '[') jarray(s);
    else if (c == '"') jstring(s);
    else if (c == 't') { jlit(s, "true", 4); push(obj_true); }
    else if (c == 'f') { jlit(s, "false", 5); push(obj_false); }
    else if (c == 'n') { jlit(s, "null", 4); push(obj_null); }
    else if (c == '-' || (c >= '0' && c <= '9')) jnumber(s);
    else type_err();
}

void prim_fromjson(void)
{
    uint16_t b = rsp;
    obj_t *t, *to;
    struct src s;
    t = force_pop_as(T_STRING, &to);
    if (CTX(t)) type_err();                     /* forceStringNoCtx */
    rpush(to);
    s.k = b;
    s.p = 0;
    s.n = AUX16(t);
    if (s.n && src_at(&s)[0] == 0xEF) {         /* nlohmann skips a BOM, and only a whole one */
        if (s.n < 3 || src_at(&s)[1] != 0xBB || src_at(&s)[2] != 0xBF) type_err();
        s.p = 3;
    }
    jvalue(&s);
    ws(&s);
    if (s.p != s.n) type_err();                 /* something after the value */
    rsp = b;
}

/* --- toXML: Nix's value-to-xml.cc and xml-writer.cc ------------------------------------- */
/* Nix writes the XML as it forces; here, as for toJSON, the tree first --
 * every value forced in Nix's order, a set's `type` forced to see whether it
 * is a derivation, a derivation's drvPath and outPath forced, and its
 * attributes only the first time its drvPath is seen (a derivation without a
 * string drvPath is always "repeated"); the strings' contexts united; and,
 * for each lambda with formals, its functionArgs set built and left on the
 * value stack in order -- then the text, which repeats the walk with no
 * bytecode: the same derivations seen in the same order, the same sets taken
 * from the stack. A lambda's parameter is the image's (BYTECODE.md "Chunk
 * kinds"); a primop is <unevaluated />. Positions are off: Nix's toXML
 * passes location = false. */

/* The chunk the LAMBDA view v runs: its kind (0 if the image has none) and
 * its info record (0 if none). */
static uint8_t lambda_kind(obj_t *v, const uint8_t **info)
{
    const uint8_t *img = (const uint8_t *)IMAGE_BASE;
    uint16_t i, kinds = img[12] | ((uint16_t)img[13] << 8), at;
    *info = 0;
    if (!kinds) return 0;
    for (i = 0; i < nchunks && chunk_info[i].code != CL_CODE(v); ++i) ;
    if (i == nchunks) return 0;
    at = img[kinds + nchunks + 2 * i] | ((uint16_t)img[kinds + nchunks + 2 * i + 1] << 8);
    if (at) *info = img + at;
    return img[kinds + i];
}

/* isDerivation on a forced set (an original): its `type` -- forced here, as
 * Nix forces it -- is the string "derivation". Runs bytecode. */
static int is_drv_forcing(obj_t *set)
{
    obj_t *x;
    uint16_t ix;
    if (!find_attr(set, str_type, &ix, &x)) return 0;
    push(x);
    force_top();
    x = deref(pop());
    return x->tag == T_STRING && IS_LIT(x, "derivation");
}

/* The same, on a set whose `type` the tree forced: no bytecode. */
static int is_drv(obj_t *set)
{
    obj_t *x, *v;
    uint16_t ix;
    if (!find_attr(set, str_type, &ix, &x)) return 0;
    for (v = deref(x); v->tag == T_INDIRECT; v = deref(x)) x = IND_TARGET(v);
    return v->tag == T_STRING && IS_LIT(v, "derivation");
}

/* A forced attribute's value, the one the C literal names, followed through
 * INDIRECTs: the STRING original, or 0. */
static obj_t *str_attr(obj_t *set, const char *lit, uint16_t n)
{
    obj_t *x = lit_attr(set, lit, n), *v;
    if (!x) return 0;
    for (v = deref(x); v->tag == T_INDIRECT; v = deref(x)) x = IND_TARGET(v);
    return v->tag == T_STRING ? x : 0;
}

/* Nix's drvPath for a derivation's XML: a string drvPath, or "" -- and ""
 * (none, or the empty string) is always "repeated". */
static int fresh_drv(obj_t *dp)
{
    return dp && AUX16(deref(dp)) != 0;
}

/* Is the drvPath (a STRING original) among the *n seen ones at
 * vstack[seen..seen+*n)? The caller adds a new one. */
static int seen_before(uint16_t seen, uint16_t *n, obj_t *drv)
{
    uint16_t i;
    for (i = 0; i < *n; ++i)
        if (str_cmp(deref(vstack[seen + i]), deref(drv)) == 1) return 1;
    return 0;
}

/* The tree. rstack[k] is the value; rstack[acc] the context; rstack[seen]
 * a LIST of the drvPaths seen (the value stack is the formals sets' log, so
 * the seen ones live elsewhere). */
static void xtree(uint16_t k, uint16_t acc, uint16_t seen)
{
    obj_t *v, *x, *o;
    uint16_t n, i, cnt, a;
    const uint8_t *info;
    stack_check();
    push(rstack[k]);
    force_top();
    rstack[k] = pop();
    v = deref(rstack[k]);
    switch (v->tag) {
    case T_STRING:
        unite(acc, rstack[k]);
        break;
    case T_LIST:
        n = AUX16(v);
        rpush(0);
        for (i = 0; i < n; ++i) { rstack[rsp - 1] = RELEM(k, i, n); xtree(rsp - 1, acc, seen); }
        --rsp;
        break;
    case T_LAMBDA:
        if (lambda_kind(v, &info) == 4 && (a = AUX16(v)) != 0) {   /* formals: log its functionArgs set */
            nursery_reserve(12);
            o = alloc(12);
            SET_HDR(o, T_THUNK, 0);
            CL_CODE(o) = chunk_info[a - 1].code;
            CL_CTAB(o) = chunk_info[a - 1].ctab;
            CL_ENV(o) = 0;
            push(o);
            force_top();
        }
        break;
    case T_ATTRS:
        if (is_drv_forcing(rstack[k])) {
            if ((x = lit_attr(rstack[k], "drvPath", 7)) != 0) { push(x); force_top(); --vsp; }
            if ((x = lit_attr(rstack[k], "outPath", 7)) != 0) { push(x); force_top(); --vsp; }
            x = str_attr(rstack[k], "drvPath", 7);
            if (!fresh_drv(x)) break;           /* "repeated": attributes unforced */
            cnt = AUX16(deref(rstack[seen]));
            for (i = 0; i < cnt; ++i)
                if (str_cmp(deref(list_elem(rstack[seen], i, cnt)), deref(x)) == 1) break;
            if (i < cnt) break;                 /* seen: "repeated" */
            push(x);                            /* the seen LIST, one longer */
            for (i = 0; i < cnt; ++i) push(list_elem(rstack[seen], i, cnt));
            rstack[seen] = list_from_stack(cnt + 1);
        }
        n = AUX16(deref(rstack[k]));
        rpush(0);
        for (i = 0; i < n; ++i) {
            rstack[rsp - 1] = AVALUES(deref(rstack[k]), n)[i];
            xtree(rsp - 1, acc, seen);
        }
        --rsp;
        break;
    default:                                    /* INT, FLOAT, BOOL, NULL, PATH */
        break;
    }
}

/* XMLWriter's attribute value: " < > & and a newline escaped. */
static void xesc(struct text *t, const char *s, uint16_t n)
{
    uint16_t i;
    for (i = 0; i < n; ++i) {
        switch (s[i]) {
        case '"':  put(t, "&quot;", 6); break;
        case '<':  put(t, "&lt;", 4); break;
        case '>':  put(t, "&gt;", 4); break;
        case '&':  put(t, "&amp;", 5); break;
        case '\n': put(t, "&#xA;", 5); break;
        default:   put(t, s + i, 1);
        }
    }
}

static void xindent(struct text *t, uint16_t depth)
{
    while (depth--) put(t, "  ", 2);
}

/* A literal and its length, known at compile time: strlen would link into
 * bank 0. */
#define LIT(s) s, (uint16_t)(sizeof(s) - 1)

/* <name a="v" />, or <name a="v"> when open; v of n bytes, a 0 for none. */
static void xelem(struct text *t, uint16_t depth, const char *name, uint16_t nl, const char *a, uint16_t al,
                  const char *v, uint16_t n, int open)
{
    xindent(t, depth);
    put(t, "<", 1);
    put(t, name, nl);
    if (a) {
        put(t, " ", 1);
        put(t, a, al);
        put(t, "=\"", 2);
        xesc(t, v, n);
        put(t, "\"", 1);
    }
    put(t, open ? ">\n" : " />\n", open ? 2 : 4);
}

static void xclose(struct text *t, uint16_t depth, const char *name, uint16_t nl)
{
    xindent(t, depth);
    put(t, "</", 2);
    put(t, name, nl);
    put(t, ">\n", 2);
}

/* The text of o at depth; seen and *nseen the drvPaths seen so far, on the
 * value stack (pushing is no allocation). */
static void xtext(struct text *t, obj_t *o, uint16_t depth, uint16_t seen, uint16_t *nseen)
{
    obj_t *v, *x, *dp, *op, *fs;
    uint16_t n, i, len;
    uint64_t bits;
    const uint8_t *info;
    char num[24];
    stack_check();
    windows_maybe();
    for (v = deref(o); v->tag == T_INDIRECT; v = deref(o)) o = IND_TARGET(v);
    switch (v->tag) {
    case T_INT:
        len = int_text(INTVAL(v), num);
        xelem(t, depth, LIT("int"), LIT("value"), num, len, 0);
        break;
    case T_BOOL:
        if (BOOLVAL(v)) xelem(t, depth, LIT("bool"), LIT("value"), "true", 4, 0);
        else xelem(t, depth, LIT("bool"), LIT("value"), "false", 5, 0);
        break;
    case T_STRING: xelem(t, depth, LIT("string"), LIT("value"), STRBYTES(v), AUX16(v), 0); break;
    case T_PATH:   xelem(t, depth, LIT("path"), LIT("value"), STRBYTES(v), AUX16(v), 0); break;
    case T_NULL:   xelem(t, depth, LIT("null"), 0, 0, 0, 0, 0); break;
    case T_FLOAT:
        memcpy(&bits, PAYLOAD(v), 8);
        stack_need(FLOAT_STACK);
        len = f_g(bits, num);
        xelem(t, depth, LIT("float"), LIT("value"), num, len, 0);
        break;
    case T_LIST:
        n = AUX16(v);
        xelem(t, depth, LIT("list"), 0, 0, 0, 0, 1);
        for (i = 0; i < n; ++i) xtext(t, list_elem(o, i, n), depth + 1, seen, nseen);
        xclose(t, depth, LIT("list"));
        break;
    case T_LAMBDA:
        switch (lambda_kind(v, &info)) {
        case 3:                                 /* x: ... */
            xelem(t, depth, LIT("function"), 0, 0, 0, 0, 1);
            xelem(t, depth + 1, LIT("varpat"), LIT("name"), (const char *)info + 3, info[1] | ((uint16_t)info[2] << 8), 0);
            xclose(t, depth, LIT("function"));
            break;
        case 4:                                 /* { a, b ? d, ... } @ s: ... */
            xelem(t, depth, LIT("function"), 0, 0, 0, 0, 1);
            xindent(t, depth + 1);
            put(t, "<attrspat", 9);
            if (info[0] & 1) put(t, " ellipsis=\"1\"", 13);     /* the attributes sorted: ellipsis, name */
            len = info[1] | ((uint16_t)info[2] << 8);
            if (len) { put(t, " name=\"", 7); xesc(t, (const char *)info + 3, len); put(t, "\"", 1); }
            put(t, ">\n", 2);
            fs = AUX16(v) ? vstack[t->next++] : 0;  /* the tree's functionArgs set */
            n = fs ? AUX16(deref(fs)) : 0;
            for (i = 0; i < n; ++i) {
                x = deref(ANAMES(deref(fs), n)[i]);
                xelem(t, depth + 2, LIT("attr"), LIT("name"), STRBYTES(x), AUX16(x), 0);
            }
            xclose(t, depth + 1, LIT("attrspat"));
            xclose(t, depth, LIT("function"));
            break;
        default:                                /* a primop, or partly applied */
            xelem(t, depth, LIT("unevaluated"), 0, 0, 0, 0, 0);
        }
        break;
    case T_ATTRS:
        if (is_drv(o)) {
            dp = str_attr(o, "drvPath", 7);
            op = str_attr(o, "outPath", 7);
            xindent(t, depth);
            put(t, "<derivation", 11);
            if (dp) { put(t, " drvPath=\"", 10); x = deref(dp); xesc(t, STRBYTES(x), AUX16(x)); put(t, "\"", 1); }
            if (op) { put(t, " outPath=\"", 10); x = deref(op); xesc(t, STRBYTES(x), AUX16(x)); put(t, "\"", 1); }
            put(t, ">\n", 2);
            if (!fresh_drv(dp) || seen_before(seen, nseen, dp)) {
                xelem(t, depth + 1, LIT("repeated"), 0, 0, 0, 0, 0);
            } else {
                vstack[seen + (*nseen)++] = dp;
                n = AUX16(deref(o));
                for (i = 0; i < n; ++i) {
                    x = deref(ANAMES(deref(o), n)[i]);
                    xelem(t, depth + 1, LIT("attr"), LIT("name"), STRBYTES(x), AUX16(x), 1);
                    xtext(t, AVALUES(deref(o), n)[i], depth + 2, seen, nseen);
                    xclose(t, depth + 1, LIT("attr"));
                }
            }
            xclose(t, depth, LIT("derivation"));
            break;
        }
        n = AUX16(deref(o));
        xelem(t, depth, LIT("attrs"), 0, 0, 0, 0, 1);
        for (i = 0; i < n; ++i) {
            x = deref(ANAMES(deref(o), n)[i]);
            xelem(t, depth + 1, LIT("attr"), LIT("name"), STRBYTES(x), AUX16(x), 1);
            xtext(t, AVALUES(deref(o), n)[i], depth + 2, seen, nseen);
            xclose(t, depth + 1, LIT("attr"));
        }
        xclose(t, depth, LIT("attrs"));
        break;
    default:
        badnbc();                               /* the tree forced everything */
    }
}

/* The whole document once: counted with t->buf 0, else written. */
static void xdoc(struct text *t, obj_t *v, uint16_t at, uint16_t seen)
{
    uint16_t nseen = 0;
    t->len = 0;
    t->next = at;
    put(t, LIT("<?xml version='1.0' encoding='utf-8'?>\n<expr>\n"));
    xtext(t, v, 1, seen, &nseen);
    put(t, LIT("</expr>\n"));
}

/* builtins.toXML v: the tree, the text counted, one STRING, the text written. */
void prim_toxml(void)
{
    uint16_t b = rsp, at, seen;
    struct text t;
    obj_t *r;
    rpush(pop());                               /* b: the value */
    nursery_reserve(16);
    rpush(str_alloc(0));                        /* b+1: the context so far */
    rpush(list_alloc(0));                       /* b+2: the drvPaths the tree has seen */
    at = vsp;
    xtree(b, b + 1, b + 2);
    seen = vsp;                                 /* the text's own seen drvPaths go here */
    if ((uint16_t)(VSTACK_N - vsp) < AUX16(deref(rstack[b + 2])) + 4) nomem();
    t.buf = 0;
    xdoc(&t, rstack[b], at, seen);
    nursery_reserve(round4(t.len) + 8);         /* the stacks hold everything */
    r = str_alloc(t.len);
    t.buf = STRBYTES(r);
    xdoc(&t, rstack[b], at, seen);
    STRCTX(r, t.len) = CTX(deref(rstack[b + 1]));
    rsp = b;
    vsp = at;
    push(r);
}

/* --- builtins.fromTOML (docs/TOML.md) --------------------------------------------------- */

/* Nix's prim_fromTOML: the string, without context, parsed as toml11 parses
 * it (toml.c), then its visit -- a table a set, an array a list, a NUL in a
 * key or a string an error (forceNoNullByte). toml.c's tree is in bank 7,
 * which no collection touches: the value is sized from it first and reserved
 * at once, so building it cannot collect, and needs no stack. */
_Noreturn void tm_fail(uint8_t why)
{
    if (why == TM_ESPACE) nomem();
    err_exit(E_TOML);
}

#define TM_BUILD_DEPTH 48       /* tables in tables, a header's keys among them */

static uint32_t tm_size(uint16_t v, uint16_t depth)
{
    uint32_t t = 0, k = 0;
    uint16_t i;
    uint8_t kind = tm_kind(v);
    if (depth > TM_BUILD_DEPTH) nomem();
    switch (kind) {
    case TM_TABLE:
    case TM_ARRAY:
        for (i = tm_first(v); i; i = tm_next(i)) {
            ++k;
            if (kind == TM_TABLE) t += (uint32_t)round4(tm_len(tm_key(i))) + 8;
            t += tm_size(tm_val(i), depth + 1);
        }
        if (!k) return t + 8;
        if (kind == TM_TABLE) {                 /* 12 bytes an attribute, and a position */
            t += round4((uint16_t)(k + k));
            k = k + k + k;                      /* (no 32-bit multiply: ~UMUL4 is bank 0's) */
        }
        return t + k + k + k + k + 4;
    case TM_STRING: return (uint32_t)round4(tm_len(v)) + 8;
    case TM_INT: case TM_FLOAT: return 12;
    default: return 0;
    }
}

/* A STRING of a tree string's bytes; a NUL is Nix's forceNoNullByte */
static obj_t *tm_str(uint16_t s)
{
    uint16_t n = tm_len(s), i;
    const uint8_t *b = tm_bytes(s);
    obj_t *o;
    for (i = 0; i != n; ++i) if (!b[i]) err_exit(E_TOML);
    o = str_alloc(n);
    memcpy(STRBYTES(o), tm_bytes(s), n);
    return o;
}

static obj_t *tm_build(uint16_t v)
{
    obj_t *o, *nm;
    uint16_t i, n = 0, k;
    uint64_t b;
    switch (tm_kind(v)) {
    case TM_TABLE:
        tm_sort(v);
        for (i = tm_first(v); i; i = tm_next(i)) ++n;
        o = attrs_alloc(n);
        for (i = tm_first(v), k = 0; i; i = tm_next(i), ++k) {
            nm = tm_str(tm_key(i));
            HASHES(o)[k] = hash_str(nm);
            ANAMES(o, n)[k] = nm;
            AVALUES(o, n)[k] = tm_build(tm_val(i));
        }
        return o;
    case TM_ARRAY:
        for (i = tm_first(v); i; i = tm_next(i)) ++n;
        o = list_alloc(n);
        for (i = tm_first(v), k = 0; i; i = tm_next(i), ++k) ELEMS(o)[k] = tm_build(tm_val(i));
        return o;
    case TM_STRING: return tm_str(v);
    case TM_INT:
        memcpy(&b, tm_bytes(v), 8);
        return new_int((int64_t)b);
    case TM_FLOAT:
        memcpy(&b, tm_bytes(v), 8);
        return new_float(b);
    default:
        if (tm_bool(v)) return obj_true;
        return obj_false;
    }
}

void prim_fromtoml(void)
{
    obj_t *s;
    uint16_t root;
    uint32_t total;
    force_top();
    s = deref(peek());
    if (s->tag != T_STRING || CTX(s)) type_err();      /* forceStringNoCtx */
    stack_need(TOML_STACK);
    root = tm_parse(STRBYTES(s), AUX16(s));
    total = tm_size(root, 0);
    if (total > 0xFFFFUL) nomem();
    nursery_reserve((uint16_t)total);
    (void)pop();
    push(tm_build(root));
}
