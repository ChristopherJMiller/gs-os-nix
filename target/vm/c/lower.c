/* lower.c -- nbc's front.rs, and the rnix accessors it calls, over the syntax
 * tree parse.c builds (docs/COMPILER.md "Stage 2b"): the AST nbc's ast.rs
 * defines, for stage 3 to compile, and an S-expression of it for the diff
 * against `nbc --ast`. Each function is front.rs's of the same name; every
 * refusal there is E_UNSUP here.
 *
 * The AST lives in an arena in banks 1 and 2 -- the nursery and the
 * collector's stage, which nothing uses before the image runs -- for one
 * compilation. Strings are copied in; nodes are 18 bytes, list cells 16.
 */
#pragma noroot
#pragma optimize 15

#include "vm.h"
#include "front.h"

segment "front";

#define ST       CST_STORE
#define XA_BASE  0x00010000UL
#define XA_END   0x00030000UL
#define SCRATCH  ((uint8_t *)FRONT_SCRATCH)         /* a string or a path being built */
#define CHAIN    ((uint8_t *)FRONT_CHAIN)           /* the files being lowered: [u16 length][path] each */
#define FIXTURE_HOME "/fake-home"

static uint32_t xa;                                 /* the arena's next free byte */
static const uint8_t *lsrc;                         /* the file being lowered */
static uint16_t llen;
static uint16_t src_top;                            /* bank 13: the end of the sources in use */
static uint16_t ch_n, ch_cur;                       /* the chain's bytes, the current file's entry */

_Noreturn static void refuse(void) { err_exit(E_UNSUP); }

/* strlen would link a library routine into bank 0 */
static uint16_t cstrlen(const char *s)
{
    uint16_t n = 0;
    while (s[n]) ++n;
    return n;
}

static void *xalloc(uint16_t n)
{
    uint32_t p = xa;
    if (XA_END - xa < n) nomem();
    xa += (n + 1) & ~1U;
    return (void *)p;
}

/* --- strings in the arena --------------------------------------------------------------- */
static struct xs *xs_new(const uint8_t *b, uint16_t n)
{
    struct xs *s = (struct xs *)xalloc((uint16_t)(n + 2));
    s->len = n;
    if (n) memcpy(XS_TEXT(s), b, n);
    return s;
}

static int xs_eq(const struct xs *a, const struct xs *b)
{
    return a->len == b->len && (a->len == 0 || memcmp(XS_TEXT(a), XS_TEXT(b), a->len) == 0);
}

static int xs_is(const struct xs *a, const char *lit)
{
    uint16_t n = (uint16_t)cstrlen(lit);
    return a->len == n && (n == 0 || memcmp(XS_TEXT(a), lit, n) == 0);
}

/* --- nodes ------------------------------------------------------------------------------------ */
static struct xn *xn_new(uint8_t tag)
{
    struct xn *n = (struct xn *)xalloc(sizeof(struct xn));
    memset(n, 0, sizeof(struct xn));
    n->tag = tag;
    return n;
}

static struct xn *xn2(uint8_t tag, void *a, void *b)
{
    struct xn *n = xn_new(tag);
    n->a = a;
    n->b = b;
    return n;
}

static struct xc *xc_new(void *a, void *b, uint16_t line, uint16_t col)
{
    struct xc *c = (struct xc *)xalloc(sizeof(struct xc));
    c->next = 0;
    c->a = a;
    c->b = b;
    c->line = line;
    c->col = col;
    return c;
}

/* Append to a list held at *head. */
static void xc_push(struct xc **head, struct xc *c)
{
    while (*head) head = &(*head)->next;
    *head = c;
}

/* --- the tree: rnix's typed accessors ------------------------------------------------------------- */
static int is_expr(uint8_t k)
{
    switch (k) {
    case NODE_APPLY: case NODE_ASSERT: case NODE_ERROR: case NODE_IF_ELSE: case NODE_SELECT:
    case NODE_STRING: case NODE_PATH_ABS: case NODE_PATH_REL: case NODE_PATH_HOME:
    case NODE_PATH_SEARCH: case NODE_LITERAL: case NODE_LAMBDA: case NODE_LEGACY_LET:
    case NODE_LET_IN: case NODE_LIST: case NODE_BIN_OP: case NODE_PAREN: case NODE_ROOT:
    case NODE_ATTR_SET: case NODE_UNARY_OP: case NODE_IDENT: case NODE_WITH: case NODE_HAS_ATTR:
    case NODE_CUR_POS:
        return 1;
    }
    return 0;
}

static int is_attr(uint8_t k) { return k == NODE_IDENT || k == NODE_DYNAMIC || k == NODE_STRING; }

#define CHILD(n, i) (&ST[(n)->a + (i)])

/* nth: the i-th child node that casts to an expression */
static const struct el *nth_expr(const struct el *n, uint16_t i)
{
    uint16_t k;
    const struct el *c;
    for (k = 0; k < n->b; ++k) {
        c = CHILD(n, k);
        if (c->kind >= NODE_FIRST && is_expr(c->kind)) {
            if (i == 0) return c;
            --i;
        }
    }
    return 0;
}

/* nth, of one node kind */
static const struct el *nth_node(const struct el *n, uint8_t kind, uint16_t i)
{
    uint16_t k;
    const struct el *c;
    for (k = 0; k < n->b; ++k) {
        c = CHILD(n, k);
        if (c->kind == kind) {
            if (i == 0) return c;
            --i;
        }
    }
    return 0;
}

/* token_u: the first child token of a kind */
static const struct el *token_of(const struct el *n, uint8_t kind)
{
    uint16_t k;
    for (k = 0; k < n->b; ++k)
        if (CHILD(n, k)->kind == kind) return CHILD(n, k);
    return 0;
}

/* A node's first byte and its end: rowan's text range */
static uint16_t el_start(const struct el *e)
{
    while (e->kind >= NODE_FIRST) {
        if (!e->b) return 0;
        e = CHILD(e, 0);
    }
    return e->a;
}

static uint16_t el_end(const struct el *e)
{
    while (e->kind >= NODE_FIRST) {
        if (!e->b) return 0;
        e = CHILD(e, e->b - 1);
    }
    return (uint16_t)(e->a + e->b);
}

/* --- positions ------------------------------------------------------------------------------- */
/* front.rs pos(): the line, and the column in characters, of a node's first
 * byte. Counted from a cache of the last one asked for, forward or back:
 * front.rs counts from the start of the file every time. */
static uint16_t p_off, p_line, p_bol;               /* an offset, its line, that line's start */

static void pos_of(uint16_t off, uint16_t *line, uint16_t *col)
{
    uint16_t i, c;
    if (off > llen) off = llen;
    while (p_off < off) {
        if (lsrc[p_off] == '\n') { ++p_line; p_bol = (uint16_t)(p_off + 1); }
        ++p_off;
    }
    while (p_off > off) {
        --p_off;
        if (lsrc[p_off] == '\n') {
            --p_line;
            for (p_bol = p_off; p_bol && lsrc[p_bol - 1] != '\n'; --p_bol) ;
        }
    }
    for (c = 0, i = p_bol; i < off; ++i)
        if ((lsrc[i] & 0xC0) != 0x80) ++c;          /* characters, not bytes */
    *line = p_line;
    *col = (uint16_t)(c + 1);
}

static void pos(const struct el *e, uint16_t *line, uint16_t *col) { pos_of(el_start(e), line, col); }

/* --- strings: rnix's normalized_parts and unescape --------------------------------------------- */
/* unescape(input, multiline) into the arena */
static struct xs *unescape(const uint8_t *in, uint16_t n, int multiline)
{
    struct xs *s = (struct xs *)xalloc(0);          /* the arena's top: grown in place */
    uint8_t *out = XS_TEXT(s);
    uint16_t i = 0, o = 0;
    uint8_t c;
    if (XA_END - xa < (uint32_t)n * 2U + 4) nomem();    /* '' + c can double a byte */
    while (i < n) {
        c = in[i++];
        if (c == '"' && !multiline) break;
        if (c == '\\' && !multiline) {
            if (i >= n) break;
            c = in[i++];
            out[o++] = c == 'n' ? '\n' : c == 'r' ? '\r' : c == 't' ? '\t' : c;
        } else if (c == '\'' && multiline) {
            if (i >= n) { out[o++] = '\''; continue; }
            c = in[i++];
            if (c == '\'') {
                if (i < n && in[i] == '\'') { ++i; out[o++] = '\''; out[o++] = '\''; }
                else if (i < n && in[i] == '$') { ++i; out[o++] = '$'; }
                else if (i < n && in[i] == '\\') {
                    ++i;
                    if (i >= n) break;
                    c = in[i++];
                    out[o++] = c == 'n' ? '\n' : c == 'r' ? '\r' : c == 't' ? '\t' : c;
                } else break;
            } else {
                out[o++] = '\'';
                out[o++] = c;
            }
        } else {
            out[o++] = c;
        }
    }
    s->len = o;
    xa += (uint32_t)((o + 3) & ~1U);
    return s;
}

/* A string node's parts: STRING_CONTENT tokens and INTERPOL nodes, with
 * normalized_parts' two passes over an indented one. Each literal into the
 * arena (xc a); each interpolation's node (xc b, a 0) for the caller. */
static struct xc *normalized_parts(const struct el *str)
{
    struct xc *head = 0;
    uint16_t k, nparts = 0, i, p, j, o, t0, t1;
    int multiline, first_is_literal = 0, is_first_literal = 1, at_sol = 1, skip;
    uint16_t min_indent = 0xFFFF, cur_indent = 0, cur_dropped = 0;
    const struct el *c, *first = 0;
    uint8_t ch;
    for (k = 0; k < str->b; ++k)
        if (CHILD(str, k)->kind < NODE_FIRST) { first = CHILD(str, k); break; }
    multiline = first && first->b == 2 && lsrc[first->a] == '\'';
    if (multiline) {
        for (k = 0; k < str->b; ++k) {
            c = CHILD(str, k);
            if (c->kind == NODE_INTERPOL) {
                if (at_sol) { at_sol = 0; if (cur_indent < min_indent) min_indent = cur_indent; }
                ++nparts;
            } else if (c->kind == TK_STRING_CONTENT) {
                t0 = c->a;
                t1 = (uint16_t)(c->a + c->b);
                if (nparts == 0) first_is_literal = 1;
                if (is_first_literal && first_is_literal) {
                    is_first_literal = 0;
                    for (p = t0; p < t1 && lsrc[p] == ' '; ++p) ;
                    if (p < t1 && lsrc[p] == '\n') t0 = (uint16_t)(p + 1);    /* a first line of spaces */
                }
                for (j = t0; j < t1; ++j) {
                    ch = lsrc[j];
                    if (at_sol) {
                        if (ch == ' ') ++cur_indent;
                        else if (ch == '\n') cur_indent = 0;
                        else { at_sol = 0; if (cur_indent < min_indent) min_indent = cur_indent; }
                    } else if (ch == '\n') {
                        at_sol = 1;
                        cur_indent = 0;
                    }
                }
                ++nparts;
            }
        }
    }
    is_first_literal = 1;
    at_sol = 1;
    for (i = 0, k = 0; k < str->b; ++k) {
        c = CHILD(str, k);
        if (c->kind == NODE_INTERPOL) {
            at_sol = 0;
            cur_dropped = 0;
            xc_push(&head, xc_new(0, (void *)c, 0, 0));
            ++i;
        } else if (c->kind == TK_STRING_CONTENT) {
            t0 = c->a;
            t1 = (uint16_t)(c->a + c->b);
            if (!multiline) {
                xc_push(&head, xc_new(unescape(lsrc + t0, (uint16_t)(t1 - t0), 0), 0, 0, 0));
                continue;
            }
            skip = 0;
            if (is_first_literal && first_is_literal) {
                is_first_literal = 0;
                for (p = t0; p < t1 && lsrc[p] == ' '; ++p) ;
                if (p < t1 && lsrc[p] == '\n') {
                    t0 = (uint16_t)(p + 1);
                    if (t0 == t1) { ++i; skip = 1; }
                }
            }
            if (skip) continue;
            for (o = 0, j = t0; j < t1; ++j) {          /* the indentation dropped, into scratch */
                ch = lsrc[j];
                if (at_sol) {
                    if (ch == ' ') {
                        if (cur_dropped >= min_indent) SCRATCH[o++] = ch;
                        ++cur_dropped;
                    } else if (ch == '\n') {
                        cur_dropped = 0;
                        SCRATCH[o++] = ch;
                    } else {
                        at_sol = 0;
                        cur_dropped = 0;
                        SCRATCH[o++] = ch;
                    }
                } else {
                    SCRATCH[o++] = ch;
                    if (ch == '\n') at_sol = 1;
                }
            }
            if (i == (uint16_t)(nparts - 1)) {          /* a last line of spaces */
                for (p = o; p > 0 && SCRATCH[p - 1] != '\n'; --p) ;
                if (p > 0) {
                    for (j = p; j < o && SCRATCH[j] == ' '; ++j) ;
                    if (j == o) o = p;
                }
            }
            xc_push(&head, xc_new(unescape(SCRATCH, o, 1), 0, 0, 0));
            ++i;
        }
    }
    return head;
}

/* --- literals ------------------------------------------------------------------------------- */
/* Integer::value(): i64's from_str of the digits; -1 past i64 */
static int int_value(const struct el *tok, int64_t *v)
{
    uint64_t x = 0;
    uint16_t i;
    uint8_t d;
    for (i = 0; i < tok->b; ++i) {
        d = (uint8_t)(lsrc[tok->a + i] - '0');
        if (x > (0x7FFFFFFFFFFFFFFFULL - d) / 10) return -1;
        x = x * 10 + d;
    }
    *v = (int64_t)x;
    return 0;
}

/* front.rs float_literal: the double, or a refusal where Nix says "invalid float" */
static uint64_t float_value(const struct el *tok)
{
    const char *t = (const char *)(lsrc + tok->a);
    uint16_t n = tok->b, i;
    uint64_t v;
    int zero = 1;
    for (i = 0; i < n && t[i] != 'e' && t[i] != 'E'; ++i)
        if (t[i] >= '1' && t[i] <= '9') zero = 0;
    if (zero) return 0;                                 /* any zero is 0, 0.0e-400 too */
    stack_need(FLOAT_STACK);
    v = f_strtod(t, n);
    if ((v >> 52) == 0x7FF) refuse();                   /* overflow */
    if (v < 0x0010000000000000ULL) refuse();             /* below 2^-1022 */
    if (v == 0x0010000000000000ULL && !f_at_least_mid(t, n)) refuse();
    return v;
}

/* --- the lowering -------------------------------------------------------------------------- */
static struct xn *lx_expr(const struct el *e);

static struct xn *x_str_lit(struct xs *s)
{
    struct xn *n = xn_new(X_STR);
    n->a = xc_new(s, 0, 0, 0);
    return n;
}

static struct xn *x_var(const char *name, uint16_t line, uint16_t col)
{
    struct xn *n = xn_new(X_VAR);
    n->a = xs_new((const uint8_t *)name, (uint16_t)cstrlen(name));
    n->line = line;
    n->col = col;
    return n;
}

static int is_set(const struct xn *n) { return n->tag == X_ATTRS || n->tag == X_DYNATTRS || n->tag == X_RECSET; }

/* set_of: AttrSet, or DynAttrSet when a name is computed */
static void set_tag(struct xn *n) { n->tag = n->b ? X_DYNATTRS : X_ATTRS; }

/* insert_binding: name = value into *binds, merging attribute-set literals */
static void insert_binding(struct xc **binds, struct xs *name, uint16_t line, uint16_t col, struct xn *value)
{
    struct xc *b, *e;
    struct xn *old, *v;
    int rec1, rec2;
    for (b = *binds; b; b = b->next)
        if (xs_eq((struct xs *)b->a, name)) break;
    if (!b) {
        xc_push(binds, xc_new(name, value, line, col));
        return;
    }
    old = (struct xn *)b->b;
    if (!is_set(old) || !is_set(value)) refuse();       /* `name` bound twice */
    rec1 = old->tag == X_RECSET;
    rec2 = value->tag == X_RECSET;
    for (e = (struct xc *)value->a; e; e = e->next) {
        v = (struct xn *)e->b;
        /* a rec set's `inherit x;` reads outside it; in a plain set that is the variable */
        if (v->tag == X_INHERIT && rec2 && !rec1) {
            v = xn2(X_VAR, v->a, 0);
            v->line = ((struct xn *)e->b)->line;
            v->col = ((struct xn *)e->b)->col;
        }
        insert_binding((struct xc **)&old->a, (struct xs *)e->a, e->line, e->col, v);
    }
    if (value->b) xc_push((struct xc **)&old->b, (struct xc *)value->b);
    if (!rec1) set_tag(old);
}

static struct xs *literal_str(const struct el *s);
static void attr_key(const struct el *attr, struct xs **name, struct xn **expr);

static struct xc *path_keys(const struct el *path)
{
    struct xc *head = 0;
    struct xs *name;
    struct xn *ex;
    uint16_t k;
    for (k = 0; k < path->b; ++k)
        if (is_attr(CHILD(path, k)->kind)) {
            attr_key(CHILD(path, k), &name, &ex);
            xc_push(&head, xc_new(name, ex, 0, 0));
        }
    return head;
}

static struct xs *static_attr(const struct el *attr)
{
    struct xs *name;
    struct xn *ex;
    attr_key(attr, &name, &ex);
    if (!name) refuse();                                /* dynamic attributes not allowed in inherit */
    return name;
}

/* bindings: the entries of a let or a set, the computed ones apart -- every
 * `name = value;` first, then every inherit, as front.rs walks them */
static void bindings(const struct el *node, int is_let, struct xc **binds, struct xc **dyns)
{
    uint16_t k, a, line, col, nk, idx, j;
    const struct el *entry, *path, *val, *from, *fe;
    struct xc *keys, *kk;
    struct xn *v, *src, *ex;
    struct xs *name;
    *binds = 0;
    *dyns = 0;
    for (k = 0; k < node->b; ++k) {
        entry = CHILD(node, k);
        if (entry->kind != NODE_ATTRPATH_VALUE) continue;
        path = nth_node(entry, NODE_ATTRPATH, 0);
        if (!path) refuse();                            /* binding without a name */
        keys = path_keys(path);
        val = nth_expr(entry, 0);
        if (!val) refuse();                             /* binding without a value */
        pos(path, &line, &col);
        v = lx_expr(val);
        /* a.b.c = v  ==  a = { b = { c = v; }; }: the rest, last first */
        for (nk = 0, kk = keys; kk; kk = kk->next) ++nk;
        for (idx = nk; idx-- > 1; ) {
            for (kk = keys, j = 0; j < idx; ++j) kk = kk->next;
            if (kk->a) {
                ex = xn_new(X_ATTRS);
                ex->a = xc_new(kk->a, v, line, col);
            } else {
                ex = xn_new(X_DYNATTRS);
                ex->b = xc_new(kk->b, v, line, col);
            }
            v = ex;
        }
        if (keys->a) insert_binding(binds, (struct xs *)keys->a, line, col, v);
        else xc_push(dyns, xc_new(keys->b, v, line, col));
    }
    for (k = 0; k < node->b; ++k) {
        entry = CHILD(node, k);
        if (entry->kind != NODE_INHERIT) continue;
        src = 0;
        from = nth_node(entry, NODE_INHERIT_FROM, 0);
        if (from) {
            fe = nth_expr(from, 0);
            if (!fe) refuse();                          /* empty inherit source */
            src = lx_expr(fe);
        }
        for (a = 0; a < entry->b; ++a) {
            if (!is_attr(CHILD(entry, a)->kind)) continue;
            name = static_attr(CHILD(entry, a));
            pos(CHILD(entry, a), &line, &col);
            if (src) {
                v = xn_new(X_SELECT);
                v->a = src;                             /* shared, where front.rs clones */
                v->b = xc_new(name, 0, 0, 0);
            } else {
                v = xn2(is_let ? X_INHERIT : X_VAR, name, 0);
                v->line = line;
                v->col = col;
            }
            insert_binding(binds, name, line, col, v);
        }
    }
}

static struct xn *rec_set(const struct el *node)
{
    struct xn *n = xn_new(X_RECSET);
    bindings(node, 1, (struct xc **)&n->a, (struct xc **)&n->b);
    return n;
}

/* attr_key: an identifier, a quoted string, or ${e}; ${"a"} is the name a */
static void attr_key(const struct el *attr, struct xs **name, struct xn **expr)
{
    const struct el *t, *e;
    *name = 0;
    *expr = 0;
    if (attr->kind == NODE_IDENT) {
        t = token_of(attr, TK_IDENT);
        if (!t) refuse();                               /* identifier `or` as attribute name */
        *name = xs_new(lsrc + t->a, t->b);
        return;
    }
    if (attr->kind == NODE_STRING) {
        *name = literal_str(attr);
        if (!*name) *expr = lx_expr(attr);
        return;
    }
    e = nth_expr(attr, 0);                              /* NODE_DYNAMIC */
    if (!e) refuse();                                   /* empty ${} attribute name */
    while (e->kind == NODE_PAREN) {
        e = nth_expr(e, 0);
        if (!e) refuse();                               /* empty parentheses */
    }
    if (e->kind == NODE_STRING && (*name = literal_str(e)) != 0) return;
    *expr = lx_expr(e);
}

/* literal_str: a string with no interpolation, its text; 0 otherwise */
static struct xs *literal_str(const struct el *s)
{
    struct xc *p, *parts = normalized_parts(s);
    uint32_t total = 0;
    struct xs *out;
    uint16_t at = 0;
    for (p = parts; p; p = p->next) {
        if (!p->a) return 0;
        total += ((struct xs *)p->a)->len;
    }
    if (total > 0xFFF0UL) nomem();
    if (parts && !parts->next) return (struct xs *)parts->a;
    out = (struct xs *)xalloc((uint16_t)(total + 2));
    out->len = (uint16_t)total;
    for (p = parts; p; p = p->next) {
        memcpy(XS_TEXT(out) + at, XS_TEXT((struct xs *)p->a), ((struct xs *)p->a)->len);
        at += ((struct xs *)p->a)->len;
    }
    return out;
}

/* canon_path, into the arena: absolute, no . or empty components, .. resolved */
static struct xs *canon_path(const uint8_t *p, uint16_t n)
{
    uint8_t *out = (uint8_t *)FRONT_CANON;
    uint16_t o = 0, i = 0, j;
    while (i < n) {
        while (i < n && p[i] == '/') ++i;
        for (j = i; j < n && p[j] != '/'; ++j) ;
        if (j - i == 0 || (j - i == 1 && p[i] == '.')) {
            ;
        } else if (j - i == 2 && p[i] == '.' && p[i + 1] == '.') {
            while (o > 0 && out[o - 1] != '/') --o;
            if (o > 0) --o;
        } else {
            out[o++] = '/';
            memcpy(out + o, p + i, (uint16_t)(j - i));
            o += (uint16_t)(j - i);
        }
        i = j;
    }
    if (!o) out[o++] = '/';
    return xs_new(out, o);
}

/* a path literal's value: joined to base if relative, then canonical */
static struct xn *path_value(uint16_t t0, uint16_t t1, const char *base)
{
    uint8_t *j = SCRATCH;
    uint16_t n = 0, b;
    struct xn *x;
    if (base) {
        b = (uint16_t)cstrlen(base);
        memcpy(j, base, b);
        n = b;
        j[n++] = '/';
    }
    memcpy(j + n, lsrc + t0, (uint16_t)(t1 - t0));
    n += (uint16_t)(t1 - t0);
    x = xn_new(X_PATH);
    x->a = canon_path(j, n);
    return x;
}

/* path_interp: the first segment as Nix's parser resolves it, then the parts */
static struct xn *path_interp(const struct el *node)
{
    struct xn *x = xn_new(X_PATHI);
    const struct el *c, *ie;
    uint16_t k, n, b;
    struct xs *s;
    uint8_t *j = SCRATCH;
    for (k = 0; k < node->b; ++k) {
        c = CHILD(node, k);
        if (c->kind == NODE_INTERPOL) {
            ie = nth_expr(c, 0);
            if (!ie) refuse();                          /* empty interpolation */
            xc_push((struct xc **)&x->a, xc_new(0, lx_expr(ie), 0, 0));
            continue;
        }
        if (k > 0) {
            xc_push((struct xc **)&x->a, xc_new(xs_new(lsrc + c->a, c->b), 0, 0, 0));
            continue;
        }
        if (node->kind == NODE_PATH_HOME) {
            b = (uint16_t)cstrlen(FIXTURE_HOME);
            memcpy(j, FIXTURE_HOME, b);
            memcpy(j + b, lsrc + c->a + 1, (uint16_t)(c->b - 1));
            s = xs_new(j, (uint16_t)(b + c->b - 1));
        } else {
            n = 0;
            if (node->kind == NODE_PATH_REL) {
                b = (uint16_t)cstrlen(FIXTURE_PWD);
                memcpy(j, FIXTURE_PWD, b);
                j[b] = '/';
                n = (uint16_t)(b + 1);
            }
            memcpy(j + n, lsrc + c->a, c->b);
            s = canon_path(j, (uint16_t)(n + c->b));
            if (c->b > 1 && lsrc[c->a + c->b - 1] == '/') {   /* its trailing '/' added back */
                struct xs *t = (struct xs *)xalloc((uint16_t)(s->len + 3));
                t->len = (uint16_t)(s->len + 1);
                memcpy(XS_TEXT(t), XS_TEXT(s), s->len);
                XS_TEXT(t)[s->len] = '/';
                s = t;
            }
        }
        xc_push((struct xc **)&x->a, xc_new(s, 0, 0, 0));
    }
    return x;
}

static int has_trivia(const struct el *n)
{
    uint16_t k;
    for (k = 0; k < n->b; ++k)
        if (CHILD(n, k)->kind == TK_WHITESPACE || CHILD(n, k)->kind == TK_COMMENT) return 1;
    return 0;
}

/* --- expr(), a case at a time: its own frame stays small, for nesting --------------------------- */
static struct xn *lx_literal(const struct el *e)
{
    const struct el *t = 0;
    uint16_t k;
    struct xn *x;
    int64_t v;
    for (k = 0; k < e->b; ++k) {
        t = CHILD(e, k);
        if (t->kind == TK_FLOAT || t->kind == TK_INTEGER || t->kind == TK_URI) break;
    }
    if (t->kind == TK_INTEGER) {
        if (int_value(t, &v)) refuse();                 /* integer literal out of range */
        x = xn_new(X_INT);
        memcpy(&x->a, &v, 8);
        return x;
    }
    if (t->kind == TK_FLOAT) {
        uint64_t bits = float_value(t);
        x = xn_new(X_FLOAT);
        memcpy(&x->a, &bits, 8);
        return x;
    }
    return x_str_lit(xs_new(lsrc + t->a, t->b));       /* a URI is a string */
}

static struct xn *lx_unary(const struct el *e)
{
    const struct el *operand = nth_expr(e, 0), *t;
    uint16_t k;
    uint8_t op = 0;
    int64_t v;
    struct xn *x;
    if (!operand) refuse();
    for (k = 0; k < e->b; ++k)
        if (CHILD(e, k)->kind == TK_SUB || CHILD(e, k)->kind == TK_INVERT) { op = CHILD(e, k)->kind; break; }
    if (op == TK_SUB) {
        /* -<int> folded, so `let x = -1;` is a literal */
        if (operand->kind == NODE_LITERAL) {
            t = token_of(operand, TK_INTEGER);
            if (t && !int_value(t, &v)) {
                x = xn_new(X_INT);
                v = -v;
                memcpy(&x->a, &v, 8);
                return x;
            }
        }
        return xn2(X_NEG, lx_expr(operand), 0);
    }
    if (op == TK_INVERT) return xn2(X_NOT, lx_expr(operand), 0);
    refuse();
    return 0;
}

static struct xn *lx_binop(const struct el *e)
{
    const struct el *l = nth_expr(e, 0), *r = nth_expr(e, 1);
    uint16_t k;
    uint8_t op = 0xFF, tag = X_BIN, bin = 0;
    struct xn *x, *a;
    if (!l || !r) refuse();
    for (k = 0; k < e->b && op == 0xFF; ++k)
        switch (CHILD(e, k)->kind) {
        case TK_CONCAT: case TK_UPDATE: case TK_ADD: case TK_SUB: case TK_MUL: case TK_DIV:
        case TK_AND_AND: case TK_EQUAL: case TK_IMPLICATION: case TK_LESS: case TK_LESS_OR_EQ:
        case TK_MORE: case TK_MORE_OR_EQ: case TK_NOT_EQUAL: case TK_OR_OR: case TK_PIPE_RIGHT:
        case TK_PIPE_LEFT:
            op = CHILD(e, k)->kind;
        }
    switch (op) {
    case TK_ADD: bin = B_ADD; break;
    case TK_SUB: bin = B_SUB; break;
    case TK_MUL: bin = B_MUL; break;
    case TK_DIV: bin = B_DIV; break;
    case TK_EQUAL: bin = B_EQ; break;
    case TK_NOT_EQUAL: bin = B_NE; break;
    case TK_LESS: bin = B_LT; break;
    case TK_LESS_OR_EQ: bin = B_LE; break;
    case TK_MORE: bin = B_GT; break;
    case TK_MORE_OR_EQ: bin = B_GE; break;
    case TK_AND_AND: tag = X_AND; break;
    case TK_OR_OR: tag = X_OR; break;
    case TK_IMPLICATION: tag = X_IMPLIES; break;
    case TK_CONCAT: tag = X_CONCAT; break;
    case TK_UPDATE: tag = X_UPDATE; break;
    default: refuse();                                  /* a pipe, or no operator */
    }
    a = lx_expr(l);
    x = xn2(tag, a, lx_expr(r));
    x->op = bin;
    return x;
}

static struct xn *lx_let(const struct el *e)
{
    struct xn *x = xn_new(X_LET);
    struct xc *dyns, *b;
    const struct el *body;
    bindings(e, 1, (struct xc **)&x->a, &dyns);
    if (dyns) refuse();                                 /* dynamic attributes not allowed in let */
    for (b = (struct xc *)x->a; b; b = b->next)
        if (xs_is((struct xs *)b->a, "import")) refuse();   /* import is shadowed */
    body = nth_expr(e, 0);
    if (!body) refuse();
    x->b = lx_expr(body);
    return x;
}

static struct xn *lx_set(const struct el *e)
{
    struct xn *x;
    if (token_of(e, TK_REC)) return rec_set(e);
    x = xn_new(X_ATTRS);
    bindings(e, 0, (struct xc **)&x->a, (struct xc **)&x->b);
    set_tag(x);
    return x;
}

static struct xn *lx_list(const struct el *e)
{
    struct xn *x = xn_new(X_LIST);
    uint16_t k;
    for (k = 0; k < e->b; ++k)
        if (CHILD(e, k)->kind >= NODE_FIRST && is_expr(CHILD(e, k)->kind))
            xc_push((struct xc **)&x->a, xc_new(lx_expr(CHILD(e, k)), 0, 0, 0));
    return x;
}

static struct xn *lx_string(const struct el *e)
{
    struct xn *x = xn_new(X_STR);
    struct xc *p;
    const struct el *ie;
    x->a = normalized_parts(e);
    for (p = (struct xc *)x->a; p; p = p->next)
        if (!p->a) {
            ie = nth_expr((const struct el *)p->b, 0);
            if (!ie) refuse();                          /* empty interpolation */
            p->b = lx_expr(ie);
        }
    return x;
}

static struct xn *lx_select(const struct el *e)
{
    const struct el *ex = nth_expr(e, 0), *path = nth_node(e, NODE_ATTRPATH, 0), *d;
    struct xn *x = xn_new(X_SELECT);
    if (!ex || !path) refuse();
    x->b = path_keys(path);
    d = nth_expr(e, 1);
    if (d) x->c = lx_expr(d);
    x->a = lx_expr(ex);
    return x;
}

static struct xn *lx_hasattr(const struct el *e)
{
    const struct el *ex = nth_expr(e, 0), *path = nth_node(e, NODE_ATTRPATH, 0);
    struct xn *x = xn_new(X_HASATTR);
    if (!ex || !path) refuse();
    x->b = path_keys(path);
    x->a = lx_expr(ex);
    return x;
}

static struct xs *ident_name(const struct el *ident)
{
    const struct el *t;
    if (!ident) refuse();
    t = token_of(ident, TK_IDENT);
    if (!t) refuse();                                   /* identifier `or` */
    return xs_new(lsrc + t->a, t->b);
}

static struct xn *lx_lambda(const struct el *e)
{
    const struct el *body = nth_expr(e, 0), *p, *en, *d, *bind;
    struct xn *x = xn_new(X_LAMBDA);
    uint16_t k, line, col;
    if (!body) refuse();
    p = nth_node(e, NODE_IDENT_PARAM, 0);
    if (!p) p = nth_node(e, NODE_PATTERN, 0);
    if (!p) refuse();                                   /* lambda without a parameter */
    if (p->kind == NODE_IDENT_PARAM) {
        x->a = ident_name(nth_node(p, NODE_IDENT, 0));
    } else {
        x->op = 1;                                      /* formals */
        for (k = 0; k < p->b; ++k) {
            en = CHILD(p, k);
            if (en->kind != NODE_PAT_ENTRY) continue;
            pos(en, &line, &col);
            d = nth_expr(en, 1);
            xc_push((struct xc **)&x->a, xc_new(ident_name(nth_node(en, NODE_IDENT, 0)), d ? lx_expr(d) : 0, line, col));
        }
        bind = nth_node(p, NODE_PAT_BIND, 0);
        if (bind) {
            d = nth_node(bind, NODE_IDENT, 0);
            if (d && token_of(d, TK_IDENT)) x->c = ident_name(d);
        }
        if (token_of(p, TK_ELLIPSIS)) x->op |= 2;
    }
    pos(p, &line, &col);
    x->line = line;
    x->col = col;
    x->b = lx_expr(body);
    return x;
}

static struct xn *lx_two(uint8_t tag, const struct el *e)
{
    const struct el *a = nth_expr(e, 0), *b = nth_expr(e, 1);
    struct xn *x;
    if (!a || !b) refuse();
    x = xn_new(tag);
    x->a = lx_expr(a);
    x->b = lx_expr(b);
    return x;
}

static struct xn *lx_if(const struct el *e)
{
    const struct el *c = nth_expr(e, 0), *t = nth_expr(e, 1), *f = nth_expr(e, 2);
    struct xn *x;
    if (!c || !t || !f) refuse();
    x = xn_new(X_IF);
    x->a = lx_expr(c);
    x->b = lx_expr(t);
    x->c = lx_expr(f);
    return x;
}

/* --- imports ----------------------------------------------------------------------------- */
#define CUR_LEN  (*(uint16_t *)(CHAIN + ch_cur))
#define CUR_PATH (CHAIN + ch_cur + 2)

static void chain_push(const uint8_t *p, uint16_t n)
{
    if ((uint16_t)(FRONT_CHAIN_MAX - ch_n) < n) nomem();
    *(uint16_t *)(CHAIN + ch_n) = n;
    memcpy(CHAIN + ch_n + 2, p, n);
    ch_cur = ch_n;
    ch_n += (uint16_t)(n + 2);
}

static int in_chain(const uint8_t *p, uint16_t n)
{
    uint16_t o = 0, l;
    while (o < ch_n) {
        l = *(uint16_t *)(CHAIN + o);
        if (l == n && memcmp(CHAIN + o + 2, p, n) == 0) return 1;
        o += (uint16_t)(l + 2);
    }
    return 0;
}

/* the import primop, applied at run time: what nbc compiles for an import
 * it cannot resolve here */
static struct xn *import_primop(const struct el *app, const struct el *arg)
{
    uint16_t line, col;
    pos(app, &line, &col);
    return xn2(X_APPLY, x_var("import", line, col), lx_expr(arg));
}

/* import <path literal>: that file read and lowered into an Import, keyed by
 * its canonical path (front.rs import) */
static struct xn *lx_import(const struct el *app, const struct el *arg)
{
    uint16_t t0, t1, i, dn, n, len, klen, s_off, s_line, s_bol, s_top, s_cur, s_n, s_llen;
    const uint8_t *key, *s_src;
    uint8_t *j = SCRATCH, *dst;
    struct cst_mark m;
    struct xn *x;
    const struct el *e;
    if (arg->kind != NODE_PATH_REL && arg->kind != NODE_PATH_ABS) return import_primop(app, arg);
    t0 = el_start(arg);
    t1 = el_end(arg);
    for (i = t0; i + 1 < t1; ++i)
        if (lsrc[i] == '$' && lsrc[i + 1] == '{') refuse();     /* interpolated import path */
    for (dn = CUR_LEN; dn > 0 && CUR_PATH[dn - 1] != '/'; --dn) ;
    if (!dn) refuse();                                  /* no file to resolve against */
    n = 0;
    if (lsrc[t0] != '/') {                              /* dir.join(text) */
        memcpy(j, CUR_PATH, dn);
        n = dn;
    }
    memcpy(j + n, lsrc + t0, (uint16_t)(t1 - t0));
    n += (uint16_t)(t1 - t0);
    dst = (uint8_t *)SRC_BASE + src_top;
    if (!gsos_import(j, n, dst, (uint16_t)(0xFF00U - src_top), &len)) return import_primop(app, arg);
    key = gsos_res(&klen);
    if (in_chain(key, klen)) refuse();                  /* import cycle */
    if (!utf8_ok(dst, len)) return import_primop(app, arg);   /* read_to_string's error */
    x = xn_new(X_IMPORT);
    x->a = xs_new(key, klen);
    s_src = lsrc; s_llen = llen; s_off = p_off; s_line = p_line; s_bol = p_bol;
    s_top = src_top; s_cur = ch_cur; s_n = ch_n;
    chain_push(XS_TEXT((struct xs *)x->a), klen);
    cst_mark(&m);
    lsrc = dst;
    llen = len;
    p_off = 0;
    p_line = 1;
    p_bol = 0;
    src_top = (uint16_t)(src_top + ((len + 1) & ~1U));
    parse_file_at(dst, len);
    e = nth_expr(cst_root(), 0);
    if (!e) refuse();                                   /* empty file */
    x->b = lx_expr(e);
    cst_release(&m);
    lsrc = s_src; llen = s_llen; p_off = s_off; p_line = s_line; p_bol = s_bol;
    src_top = s_top; ch_cur = s_cur; ch_n = s_n;
    return x;
}

static struct xn *lx_apply(const struct el *e)
{
    const struct el *f = nth_expr(e, 0), *a = nth_expr(e, 1), *t;
    if (!f || !a) refuse();
    if (f->kind == NODE_IDENT && (t = token_of(f, TK_IDENT)) != 0 && t->b == 6 &&
        memcmp(lsrc + t->a, "import", 6) == 0)
        return lx_import(e, a);
    return lx_two(X_APPLY, e);
}

static struct xn *lx_path(const struct el *e)
{
    if (has_trivia(e)) refuse();                        /* whitespace inside an interpolated path */
    if (e->b > 1) return path_interp(e);
    if (e->kind == NODE_PATH_ABS) return path_value(el_start(e), el_end(e), 0);
    if (e->kind == NODE_PATH_REL) return path_value(el_start(e), el_end(e), FIXTURE_PWD);
    return path_value((uint16_t)(el_start(e) + 1), el_end(e), FIXTURE_HOME);   /* ~ trimmed */
}

static struct xn *lx_search(const struct el *e)
{
    uint16_t t0 = el_start(e), t1 = el_end(e), line, col;
    struct xn *f, *ap;
    while (t0 < t1 && lsrc[t0] == '<') ++t0;
    while (t1 > t0 && lsrc[t1 - 1] == '>') --t1;
    pos(e, &line, &col);
    f = xn2(X_APPLY, x_var("__findFile", line, col), x_var("__nixPath", line, col));
    ap = xn2(X_APPLY, f, x_str_lit(xs_new(lsrc + t0, (uint16_t)(t1 - t0))));
    return ap;
}

static struct xn *x_int(int64_t v)
{
    struct xn *x = xn_new(X_INT);
    memcpy(&x->a, &v, 8);
    return x;
}

static struct xn *lx_cur_pos(const struct el *e)
{
    uint16_t line, col, n;
    struct xn *x = xn_new(X_ATTRS);
    uint16_t b, l = CUR_LEN;
    uint8_t *j = SCRATCH;
    pos(e, &line, &col);
    n = (uint16_t)cstrlen(FIXTURE_PWD);
    memcpy(j, FIXTURE_PWD, n);
    for (b = l; b > 0 && CUR_PATH[b - 1] != '/'; --b) ;    /* the file's name: its last component */
    if (b < l) {
        j[n++] = '/';
        memcpy(j + n, CUR_PATH + b, (uint16_t)(l - b));
        n += (uint16_t)(l - b);
    }
    /* Nix's ExprPos: its own attributes have no positions, line 0 */
    xc_push((struct xc **)&x->a, xc_new(xs_new((const uint8_t *)"column", 6), x_int(col), 0, 0));
    xc_push((struct xc **)&x->a, xc_new(xs_new((const uint8_t *)"file", 4), x_str_lit(xs_new(j, n)), 0, 0));
    xc_push((struct xc **)&x->a, xc_new(xs_new((const uint8_t *)"line", 4), x_int(line), 0, 0));
    return x;
}

static struct xn *lx_expr(const struct el *e)
{
    struct xn *x;
    uint16_t line, col;
    const struct el *t;
    stack_check();                                      /* nesting is recursion: E_NOMEM, never an overflow */
    switch (e->kind) {
    case NODE_LITERAL:    return lx_literal(e);
    case NODE_IDENT:
        t = token_of(e, TK_IDENT);
        if (!t) refuse();                               /* identifier `or` */
        pos(e, &line, &col);
        x = xn2(X_VAR, xs_new(lsrc + t->a, t->b), 0);
        x->line = line;
        x->col = col;
        return x;
    case NODE_PAREN:
        t = nth_expr(e, 0);
        if (!t) refuse();
        return lx_expr(t);
    case NODE_UNARY_OP:   return lx_unary(e);
    case NODE_BIN_OP:     return lx_binop(e);
    case NODE_IF_ELSE:    return lx_if(e);
    case NODE_LET_IN:     return lx_let(e);
    case NODE_ATTR_SET:   return lx_set(e);
    case NODE_LIST:       return lx_list(e);
    case NODE_STRING:     return lx_string(e);
    case NODE_SELECT:     return lx_select(e);
    case NODE_HAS_ATTR:   return lx_hasattr(e);
    case NODE_LAMBDA:     return lx_lambda(e);
    case NODE_WITH:       return lx_two(X_WITH, e);
    case NODE_LEGACY_LET:                               /* (rec { ... }).body */
        x = xn_new(X_SELECT);
        x->a = rec_set(e);
        x->b = xc_new(xs_new((const uint8_t *)"body", 4), 0, 0, 0);
        return x;
    case NODE_APPLY:      return lx_apply(e);
    case NODE_ASSERT:     return lx_two(X_ASSERT, e);
    case NODE_PATH_ABS: case NODE_PATH_REL: case NODE_PATH_HOME: return lx_path(e);
    case NODE_PATH_SEARCH: return lx_search(e);
    case NODE_CUR_POS:    return lx_cur_pos(e);
    }
    refuse();                                           /* a nested root, or a syntax error */
    return 0;
}

/* The whole file: the AST of the source at SRC_BASE, lowered (parse_file first). */
struct xn *lower_file(const uint8_t *text, uint16_t n)
{
    const struct el *e;
    xa = XA_BASE;
    lsrc = text;
    llen = n;
    p_off = 0;
    p_line = 1;
    p_bol = 0;
    src_top = (uint16_t)((n + 1) & ~1U);
    ch_n = 0;
    chain_push((const uint8_t *)SRC_NAME, cstrlen((const char *)SRC_NAME));
    e = nth_expr(cst_root(), 0);
    if (!e) refuse();                                   /* empty file */
    return lx_expr(e);
}

/* The arena after the AST, for the compiler (compile.c): what it builds
 * lives as long as the AST it compiles. */
void *front_alloc(uint16_t n) { return xalloc(n); }

/* A text with no file -- derivation.nix, which the compiler embeds and
 * lowers when a program first names `derivation` (front.rs parse) -- its
 * tree after the trees built, its AST after the arena's, the lowering's
 * state as it was after. */
struct xn *lower_text(const uint8_t *text, uint16_t n)
{
    const uint8_t *s_src = lsrc;
    uint16_t s_llen = llen, s_off = p_off, s_line = p_line, s_bol = p_bol, s_cur = ch_cur, s_n = ch_n;
    struct cst_mark m;
    const struct el *e;
    struct xn *x;
    chain_push((const uint8_t *)"", 0);                 /* no file: no directory, no name */
    cst_mark(&m);
    lsrc = text;
    llen = n;
    p_off = 0;
    p_line = 1;
    p_bol = 0;
    parse_file_at(text, n);
    e = nth_expr(cst_root(), 0);
    if (!e) refuse();                                   /* empty file */
    x = lx_expr(e);
    cst_release(&m);
    lsrc = s_src; llen = s_llen; p_off = s_off; p_line = s_line; p_bol = s_bol;
    ch_cur = s_cur; ch_n = s_n;
    return x;
}

/* --- the S-expression (sexp.rs) ------------------------------------------------------------------- */
static uint8_t *so;                                     /* the output's next byte, in the image bank */
#define SO_END ((uint8_t *)(IMAGE_BASE + 0xFFF0UL))

static void s_byte(uint8_t c) { if (so >= SO_END) nomem(); *so++ = c; }
static void s_lit(const char *s) { while (*s) s_byte((uint8_t)*s++); }

static void s_u64(uint64_t v)
{
    char d[21];
    int i = 20;
    uint64_t q;
    d[i] = 0;
    do { q = v / 10; d[--i] = (char)('0' + (int)(v - q * 10)); v = q; } while (v);   /* % links ~UMOD8 */
    s_lit(d + i);
}

static void s_hex(uint8_t b)
{
    s_byte((uint8_t)"0123456789ABCDEF"[b >> 4]);
    s_byte((uint8_t)"0123456789ABCDEF"[b & 15]);
}

static void s_q(const struct xs *s)
{
    uint16_t i;
    uint8_t b;
    s_byte('"');
    for (i = 0; i < s->len; ++i) {
        b = XS_TEXT(s)[i];
        if (b >= 0x20 && b < 0x7F && b != '"' && b != '\\') s_byte(b);
        else { s_lit("\\x"); s_hex(b); }
    }
    s_byte('"');
}

static void s_pos(uint16_t line, uint16_t col)
{
    s_u64(line);
    s_byte(':');
    s_u64(col);
}

static void s_expr(const struct xn *x);

/* An import's key relative to the compiled file's directory (sexp.rs
 * relative): the components they share dropped, a .. for each of the
 * directory's left. The chain's first entry is the compiled file. */
static const struct xs *relative(const struct xs *key)
{
    struct xs *r = (struct xs *)(SCRATCH + 0x6000U);
    const uint8_t *k = XS_TEXT(key), *b = CHAIN + 2;
    uint16_t kn = key->len, bn, ki = 0, bi = 0, o = 0, kc, bc, cut_k, cut_b;
    for (bn = *(uint16_t *)CHAIN; bn > 0 && b[bn - 1] != '/'; --bn) ;  /* the directory, with its '/' */
    if (bn) --bn;
    for (;;) {                                          /* the shared components */
        while (ki < kn && k[ki] == '/') ++ki;
        while (bi < bn && b[bi] == '/') ++bi;
        for (kc = ki; kc < kn && k[kc] != '/'; ++kc) ;
        for (bc = bi; bc < bn && b[bc] != '/'; ++bc) ;
        if (bi == bn || kc - ki != bc - bi || memcmp(k + ki, b + bi, (uint16_t)(kc - ki))) break;
        ki = kc;
        bi = bc;
    }
    cut_k = ki;
    cut_b = bi;
    while (cut_b < bn) {                                /* a .. for each of the directory's left */
        while (cut_b < bn && b[cut_b] == '/') ++cut_b;
        if (cut_b == bn) break;
        while (cut_b < bn && b[cut_b] != '/') ++cut_b;
        if (o) XS_TEXT(r)[o++] = '/';
        XS_TEXT(r)[o++] = '.';
        XS_TEXT(r)[o++] = '.';
    }
    while (cut_k < kn) {
        while (cut_k < kn && k[cut_k] == '/') ++cut_k;
        if (cut_k == kn) break;
        if (o) XS_TEXT(r)[o++] = '/';
        while (cut_k < kn && k[cut_k] != '/') XS_TEXT(r)[o++] = k[cut_k++];
    }
    r->len = o;
    return r;
}

static void s_binds(const struct xc *b)
{
    s_byte('(');
    for (; b; b = b->next) {
        s_byte('(');
        s_q((const struct xs *)b->a);
        s_byte(' ');
        s_pos(b->line, b->col);
        s_byte(' ');
        s_expr((const struct xn *)b->b);
        s_byte(')');
        if (b->next) s_byte(' ');
    }
    s_byte(')');
}

static void s_dyns(const struct xc *d)
{
    s_byte('(');
    for (; d; d = d->next) {
        s_byte('(');
        s_expr((const struct xn *)d->a);
        s_byte(' ');
        s_pos(d->line, d->col);
        s_byte(' ');
        s_expr((const struct xn *)d->b);
        s_byte(')');
        if (d->next) s_byte(' ');
    }
    s_byte(')');
}

static void s_parts(const char *head, const struct xc *p)
{
    s_byte('(');
    s_lit(head);
    for (; p; p = p->next) {
        s_byte(' ');
        if (p->a) {
            s_q((const struct xs *)p->a);
        } else {
            s_lit("(interp ");
            s_expr((const struct xn *)p->b);
            s_byte(')');
        }
    }
    s_byte(')');
}

static void s_keys(const struct xc *k)
{
    s_byte('(');
    for (; k; k = k->next) {
        if (k->a) {
            s_q((const struct xs *)k->a);
        } else {
            s_lit("(expr ");
            s_expr((const struct xn *)k->b);
            s_byte(')');
        }
        if (k->next) s_byte(' ');
    }
    s_byte(')');
}

static void s_node(const char *head, const struct xn *a, const struct xn *b, const struct xn *c)
{
    s_byte('(');
    s_lit(head);
    s_byte(' ');
    s_expr(a);
    if (b) { s_byte(' '); s_expr(b); }
    if (c) { s_byte(' '); s_expr(c); }
    s_byte(')');
}

static const char *bin_name(uint8_t op)
{
    switch (op) {
    case B_ADD: return "add";
    case B_SUB: return "sub";
    case B_MUL: return "mul";
    case B_DIV: return "div";
    case B_EQ: return "eq";
    case B_NE: return "ne";
    case B_LT: return "lt";
    case B_LE: return "le";
    case B_GT: return "gt";
    }
    return "ge";
}

static void s_expr(const struct xn *x)
{
    const struct xc *c;
    int64_t v;
    uint64_t bits;
    uint16_t i;
    stack_check();
    switch (x->tag) {
    case X_INT:
        memcpy(&v, &x->a, 8);
        s_lit("(int ");
        if (v < 0) { s_byte('-'); s_u64((uint64_t)0 - (uint64_t)v); }
        else s_u64((uint64_t)v);
        s_byte(')');
        return;
    case X_FLOAT:
        memcpy(&bits, &x->a, 8);
        s_lit("(float ");
        for (i = 8; i-- > 0; ) s_hex(((const uint8_t *)&bits)[i]);
        s_byte(')');
        return;
    case X_VAR:
        s_lit("(var ");
        s_q((const struct xs *)x->a);
        s_byte(' ');
        s_pos(x->line, x->col);
        s_byte(')');
        return;
    case X_INHERIT:
        s_lit("(inherit ");
        s_q((const struct xs *)x->a);
        s_byte(' ');
        s_pos(x->line, x->col);
        s_byte(')');
        return;
    case X_NEG: s_node("neg", (const struct xn *)x->a, 0, 0); return;
    case X_NOT: s_node("not", (const struct xn *)x->a, 0, 0); return;
    case X_BIN: s_node(bin_name(x->op), (const struct xn *)x->a, (const struct xn *)x->b, 0); return;
    case X_AND: s_node("and", (const struct xn *)x->a, (const struct xn *)x->b, 0); return;
    case X_OR: s_node("or", (const struct xn *)x->a, (const struct xn *)x->b, 0); return;
    case X_IMPLIES: s_node("implies", (const struct xn *)x->a, (const struct xn *)x->b, 0); return;
    case X_IF: s_node("if", (const struct xn *)x->a, (const struct xn *)x->b, (const struct xn *)x->c); return;
    case X_WITH: s_node("with", (const struct xn *)x->a, (const struct xn *)x->b, 0); return;
    case X_APPLY: s_node("apply", (const struct xn *)x->a, (const struct xn *)x->b, 0); return;
    case X_ASSERT: s_node("assert", (const struct xn *)x->a, (const struct xn *)x->b, 0); return;
    case X_UPDATE: s_node("update", (const struct xn *)x->a, (const struct xn *)x->b, 0); return;
    case X_CONCAT: s_node("concat", (const struct xn *)x->a, (const struct xn *)x->b, 0); return;
    case X_SEQ: s_node("seq", (const struct xn *)x->a, (const struct xn *)x->b, 0); return;
    case X_LET:
        s_lit("(let ");
        s_binds((const struct xc *)x->a);
        s_byte(' ');
        s_expr((const struct xn *)x->b);
        s_byte(')');
        return;
    case X_LAMBDA:
        s_lit("(lambda ");
        if (!(x->op & 1)) {
            s_lit("(ident ");
            s_q((const struct xs *)x->a);
            s_byte(')');
        } else {
            s_lit("(formals (");
            for (c = (const struct xc *)x->a; c; c = c->next) {
                s_byte('(');
                s_q((const struct xs *)c->a);
                s_byte(' ');
                s_pos(c->line, c->col);
                s_byte(' ');
                if (c->b) s_expr((const struct xn *)c->b);
                else s_lit("nil");
                s_byte(')');
                if (c->next) s_byte(' ');
            }
            s_lit(x->op & 2 ? ") t " : ") nil ");
            if (x->c) s_q((const struct xs *)x->c);
            else s_lit("nil");
            s_byte(')');
        }
        s_byte(' ');
        s_pos(x->line, x->col);
        s_byte(' ');
        s_expr((const struct xn *)x->b);
        s_byte(')');
        return;
    case X_STR: s_parts("str", (const struct xc *)x->a); return;
    case X_PATHI: s_parts("pathi", (const struct xc *)x->a); return;
    case X_PATH:
        s_lit("(path ");
        s_q((const struct xs *)x->a);
        s_byte(')');
        return;
    case X_LIST:
        s_lit("(list");
        for (c = (const struct xc *)x->a; c; c = c->next) {
            s_byte(' ');
            s_expr((const struct xn *)c->a);
        }
        s_byte(')');
        return;
    case X_ATTRS:
        s_lit("(attrs ");
        s_binds((const struct xc *)x->a);
        s_byte(')');
        return;
    case X_DYNATTRS: case X_RECSET:
        s_lit(x->tag == X_RECSET ? "(recset " : "(dynattrs ");
        s_binds((const struct xc *)x->a);
        s_byte(' ');
        s_dyns((const struct xc *)x->b);
        s_byte(')');
        return;
    case X_SELECT:
        s_lit("(select ");
        s_expr((const struct xn *)x->a);
        s_byte(' ');
        s_keys((const struct xc *)x->b);
        s_byte(' ');
        if (x->c) s_expr((const struct xn *)x->c);
        else s_lit("nil");
        s_byte(')');
        return;
    case X_HASATTR:
        s_lit("(hasattr ");
        s_expr((const struct xn *)x->a);
        s_byte(' ');
        s_keys((const struct xc *)x->b);
        s_byte(')');
        return;
    case X_IMPORT:
        s_lit("(import ");
        s_q(relative((const struct xs *)x->a));
        s_byte(' ');
        s_expr((const struct xn *)x->b);
        s_byte(')');
        return;
    }
    s_lit("(?)");
}

/* --ast (a2min): the source at SRC_BASE parsed, lowered and printed into the
 * image bank as text, its length at RESULT_ADDR */
void ast_dump(void)
{
    uint16_t n = *(volatile uint16_t *)SRC_LEN_ADDR;
    struct xn *x;
    parse_file((const uint8_t *)SRC_BASE, n);
    x = lower_file((const uint8_t *)SRC_BASE, n);
    so = (uint8_t *)IMAGE_BASE;
    s_expr(x);
    *(uint16_t *)RESULT_ADDR = (uint16_t)((uint32_t)so - IMAGE_BASE);
}
