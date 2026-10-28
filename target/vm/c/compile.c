/* compile.c -- nbc's compiler.rs and its image writer, nbc.rs write_program,
 * over the AST lower.c builds (docs/COMPILER.md "Stage 3"): the same chunks
 * in the same order, the same constants and code, the same image, byte for
 * byte. Each function is compiler.rs's of the same name and emits in its
 * order -- a chunk is numbered when it is claimed -- and every refusal there
 * is E_UNSUP here.
 *
 * The chunks' code is in bank 7, in 32-byte blocks (GS/OS's path work there
 * is over once the lowering has read every import); the chunk table,
 * constants, scopes and positions in the AST's arena; the registers at the
 * top of bank 12, so that they cost bank 0 nothing.
 */
#pragma noroot
#pragma optimize 15

#include "vm.h"
#include "front.h"
#include "opcodes.h"
#include "nbc_tables.h"

segment "compile";

#define KIND_CODE       0
#define KIND_PRIMOP     1
#define KIND_PRIMOP_APP 2
#define KIND_LAMBDA     3
#define KIND_FORMALS    4

#define CHUNKS_N  512U              /* twice the corpus's most (252); the VM loads 256 */
#define SCOPES_N  512U              /* nesting: the C stack ends it sooner */
#define BLK_DATA  30U               /* a code block: the next block's number, then 30 bytes */
#define NBLOCKS   2048U             /* 32 bytes each: bank 7 */
#define BLOCK(b)  ((uint8_t *)(0x00070000UL + (uint16_t)((uint16_t)(b) << 5)))
#define NEXT(b)   (*(uint16_t *)BLOCK(b))
#define NO_ARGS   0xFFFFU

#define BUILTINS_KEY   "<builtins>"                     /* no file is named so */
#define DERIVATION_KEY "<nix/derivation-internal.nix>"  /* Nix's corepkgs file */

struct chunk {
    uint8_t kind, ellipsis;
    const struct xs *info;              /* a lambda body's parameter name, or 0 */
    struct xc *consts;                  /* a: the value (an xs, or 8 bytes); line: its K_ kind */
    uint16_t nconsts, len, first, last, tail;   /* tail: the bytes in the last block */
    uint16_t last_call;                 /* the last CALL's position + 1, or 0 */
    uint8_t pad[10];                    /* to 32 bytes: an index is a shift, not ~CUMUL2 */
};
#ifdef __ORCAC__
_Static_assert(sizeof(struct chunk) == 32, "a chunk's entry is 32 bytes: CH() shifts");
#endif
#define CH(ch) ((struct chunk *)((uint8_t *)CS.chunks + (uint16_t)((uint16_t)(ch) << 5)))

/* An environment at run time: a let's names or a lambda's (cells whose a is
 * a name), or a `with`'s, which has none */
struct scope { struct xc *names; uint16_t with; };

/* A map is cells whose a is a key (an xs) and line a chunk. */
struct cstate {
    struct chunk *chunks;
    struct scope *scopes;
    struct xc *imported, *builtins, *files, *positions, *pos_tail;
    const uint8_t *main_dir;            /* the compiled file's directory, no '/' last */
    uint8_t *io;                        /* the image's next byte */
    uint16_t nchunk, nblock, nscope, sbase, nfiles, npositions, main_dir_n;
    int16_t cur_file;                   /* an index into files, or -1 */
};
#define CS (*(struct cstate *)FRONT_CSTATE)
_Static_assert(sizeof(struct cstate) <= 0x100, "the compiler's registers fit the top of bank 12");

_Noreturn static void refuse(void) { err_exit(E_UNSUP); }

/* strlen would link a library routine into bank 0 */
static uint16_t cslen(const char *s)
{
    uint16_t n = 0;
    while (s[n]) ++n;
    return n;
}

static void *zalloc(uint16_t n)
{
    void *p = front_alloc(n);
    memset(p, 0, n);
    return p;
}

static const struct xs *xs_of(const uint8_t *b, uint16_t n)
{
    struct xs *s = (struct xs *)front_alloc((uint16_t)(n + 2));
    s->len = n;
    if (n) memcpy(XS_TEXT(s), b, n);
    return s;
}
#define XS_LIT(lit) xs_of((const uint8_t *)(lit), (uint16_t)(sizeof(lit) - 1))

static int is(const struct xs *s, const uint8_t *b, uint16_t n)
{
    return s->len == n && (n == 0 || memcmp(XS_TEXT(s), b, n) == 0);
}
/* the lengths compared first, without a call */
#define IS(s, lit) ((s)->len == sizeof(lit) - 1 && is((s), (const uint8_t *)(lit), (uint16_t)(sizeof(lit) - 1)))
#define XS_EQ(s, t) ((s)->len == (t)->len && is((s), XS_TEXT(t), (t)->len))

static struct xc *cell(void *a, void *b, uint16_t line, uint16_t col)
{
    struct xc *c = (struct xc *)front_alloc(sizeof(struct xc));
    c->next = 0;
    c->a = a;
    c->b = b;
    c->line = line;
    c->col = col;
    return c;
}

static struct xn *mk(uint8_t tag, void *a, void *b, void *c)
{
    struct xn *n = (struct xn *)zalloc(sizeof(struct xn));
    n->tag = tag;
    n->a = a;
    n->b = b;
    n->c = c;
    return n;
}

static uint16_t count(const struct xc *l)
{
    uint16_t n = 0;
    for (; l; l = l->next) {
        if (n == 0xFFFFU) refuse();                 /* more than 65535 */
        ++n;
    }
    return n;
}

/* --- nbc's tables (nbc_tables.h) ------------------------------------------------------- */
/* An entry is its length byte, its bytes and a NUL; a zero length ends a
 * table. A name found is its bytes, NUL-terminated, its length before it. */
#define TLEN(t) ((uint16_t)(uint8_t)(t)[-1])

/* The entry equal to b[0..n), or 0; `skip` bytes follow each entry's NUL. */
static const char *t_find(const char *t, const uint8_t *b, uint16_t n, uint16_t skip)
{
    uint16_t l;
    while ((l = (uint8_t)*t) != 0) {
        if (l == n && (uint8_t)t[1] == b[0] && memcmp(t + 1, b, n) == 0) return t + 1;
        t += l + 2 + skip;
    }
    return 0;
}

static const char *primop(const uint8_t *b, uint16_t n) { return t_find(PRIMOP_TABLE, b, n, 2); }
static int nix_builtin(const uint8_t *b, uint16_t n) { return t_find(NIX_BUILTIN_TABLE, b, n, 0) != 0; }

/* A value builtin: its tag -- S a string, I a decimal, L the empty list -- and text */
static const char *builtin_value(const uint8_t *b, uint16_t n)
{
    const char *t = VALUE_TABLE, *v;
    uint16_t l;
    while ((l = (uint8_t)*t) != 0) {
        v = t + l + 2;                              /* the value's entry */
        if (l == n && memcmp(t + 1, b, n) == 0) return v + 1;
        t = v + (uint8_t)*v + 2;
    }
    return 0;
}

/* GLOBALS, or prefixed_global: `__NAME` for any other builtin. The
 * builtin's name, or 0. */
static const char *global(const uint8_t *b, uint16_t n)
{
    const char *g = t_find(GLOBAL_TABLE, b, n, 0);
    if (g) return g;
    if (n < 2 || b[0] != '_' || b[1] != '_') return 0;
    b += 2;
    n -= 2;
    if (t_find(GLOBAL_TABLE, b, n, 0) || t_find("\004true\000\005false\000\004null\000\010builtins\000", b, n, 0))
        return 0;
    return t_find(NIX_BUILTIN_TABLE, b, n, 0);
}

/* --- chunks -------------------------------------------------------------------------------- */
static uint16_t new_block(void)
{
    if (CS.nblock >= NBLOCKS) nomem();
    return CS.nblock++;
}

static uint16_t new_chunk(uint8_t kind)
{
    struct chunk *c;
    if (CS.nchunk >= CHUNKS_N) nomem();
    c = CH(CS.nchunk);
    memset(c, 0, sizeof(struct chunk));
    c->kind = kind;
    c->first = c->last = new_block();
    return CS.nchunk++;
}

static uint8_t *code_at(const struct chunk *c, uint16_t pos)
{
    uint16_t b = c->first;
    while (pos >= BLK_DATA) {
        b = NEXT(b);
        pos -= BLK_DATA;
    }
    return BLOCK(b) + 2 + pos;
}

static void emit(uint16_t ch, uint8_t byte)
{
    struct chunk *c = CH(ch);
    uint16_t nb;
    if (c->len == 0xFFFFU) nomem();                 /* no image could hold it */
    if (c->tail == BLK_DATA) {
        nb = new_block();
        NEXT(c->last) = nb;
        c->last = nb;
        c->tail = 0;
    }
    BLOCK(c->last)[2 + c->tail] = byte;
    ++c->tail;
    ++c->len;
}

static void emit_u16(uint16_t ch, uint16_t v)
{
    emit(ch, (uint8_t)v);
    emit(ch, (uint8_t)(v >> 8));
}

/* RETURN, turning a CALL immediately before it into a TAIL_CALL */
static void emit_return(uint16_t ch)
{
    struct chunk *c = CH(ch);
    if (c->len && c->last_call == c->len) *code_at(c, (uint16_t)(c->len - 1)) = OP_TAIL_CALL;
    emit(ch, OP_RETURN);
}

/* A jump with a placeholder offset; the position to patch */
static uint16_t emit_jump(uint16_t ch, uint8_t op)
{
    uint16_t at;
    emit(ch, op);
    at = CH(ch)->len;
    emit_u16(ch, 0);
    return at;
}

/* The jump at `at` lands at the chunk's end, relative to the jump's end */
static void patch_jump(uint16_t ch, uint16_t at)
{
    const struct chunk *c = CH(ch);
    uint16_t rel = (uint16_t)(c->len - (at + 2));
    if (rel > 0x7FFFU) refuse();                    /* jump offset out of range */
    *code_at(c, at) = (uint8_t)rel;
    *code_at(c, (uint16_t)(at + 1)) = (uint8_t)(rel >> 8);
}

static void emit_call(uint16_t ch)
{
    emit(ch, OP_CALL);
    CH(ch)->last_call = CH(ch)->len;
}

/* --- constants ------------------------------------------------------------------------------ */
/* The index of an equal constant, or of v added: K_INT or K_FLOAT and 8
 * bytes, or K_STRING or K_PATH and an xs. v must outlive the compilation.
 * The first equal one, as nbc's search finds it; a cell's col is a key --
 * the kind, and a number's first two bytes or a string's length and first
 * byte -- so that most of the cells it passes cost one comparison. */
static uint16_t constant(uint16_t ch, uint8_t kind, const void *v)
{
    struct chunk *c = CH(ch);
    struct xc **at = &c->consts, *k;
    const struct xs *s = (const struct xs *)v;
    const uint8_t *b = (const uint8_t *)v;
    uint16_t i = 0, key;
    int num = kind == K_INT || kind == K_FLOAT;
    if (num) key = (uint16_t)(((uint16_t)kind << 12) ^ b[0] ^ ((uint16_t)b[1] << 4));
    else key = (uint16_t)(((uint16_t)kind << 12) ^ s->len ^ (s->len ? (uint16_t)XS_TEXT(s)[0] << 4 : 0));
    for (; *at; at = &(*at)->next, ++i) {
        k = *at;
        if (k->col != key || k->line != kind) continue;
        if (num) {
            if (memcmp(k->a, v, 8) == 0) return i;
        } else if (XS_EQ((const struct xs *)k->a, s)) {
            return i;
        }
    }
    if (c->nconsts == 0xFFFFU) refuse();            /* more than 65535 constants in one chunk */
    *at = cell((void *)v, 0, kind, key);
    ++c->nconsts;
    return i;
}

static void emit_const(uint16_t ch, uint8_t kind, const void *v)
{
    uint16_t i = constant(ch, kind, v);
    emit(ch, OP_CONST);
    emit_u16(ch, i);
}

static void emit_str(uint16_t ch, const struct xs *s) { emit_const(ch, K_STRING, s); }

/* --- scopes and names ----------------------------------------------------------------------- */
/* The scopes in use are [sbase, nscope): a closed thunk is compiled with
 * none around it (compiler.rs takes them). */
static void scope_push(struct xc *names, uint16_t with)
{
    if (CS.nscope >= SCOPES_N) nomem();
    CS.scopes[CS.nscope].names = names;
    CS.scopes[CS.nscope].with = with;
    ++CS.nscope;
}

enum { R_ENV, R_LITERAL, R_GLOBAL, R_WITH, R_BUILTINS };
struct binding { uint16_t kind, depth, index; const char *global; };   /* a literal: depth its opcode */

/* resolve_from: ignoring the innermost `skip` scopes, lexical bindings, then
 * true/false/null, then -- a `with` in scope -- a run-time lookup with the
 * global as its fallback, then the globals, then `builtins` itself */
static void resolve_from(const struct xs *name, uint16_t skip, struct binding *r)
{
    uint16_t d, i, top = (uint16_t)(CS.nscope - CS.sbase);
    const struct xc *n;
    for (d = skip; d < top; ++d)
        for (i = 1, n = CS.scopes[CS.nscope - 1 - d].names; n; n = n->next, ++i)
            if (XS_EQ((const struct xs *)n->a, name)) {
                r->kind = R_ENV;
                r->depth = d;
                r->index = i;
                return;
            }
    r->kind = R_LITERAL;
    if (IS(name, "true")) { r->depth = OP_TRUE; return; }
    if (IS(name, "false")) { r->depth = OP_FALSE; return; }
    if (IS(name, "null")) { r->depth = OP_NULL; return; }
    r->global = global(XS_TEXT(name), name->len);
    for (d = skip; d < top; ++d)
        if (CS.scopes[CS.nscope - 1 - d].with) { r->kind = R_WITH; return; }
    if (r->global) { r->kind = R_GLOBAL; return; }
    if (IS(name, "builtins")) { r->kind = R_BUILTINS; return; }
    refuse();                                       /* unbound variable */
}

/* Could `name` mean something other than the global? A `with` might. */
static int is_shadowed(const struct xs *name)
{
    uint16_t s;
    const struct xc *n;
    for (s = CS.sbase; s < CS.nscope; ++s) {
        if (CS.scopes[s].with) return 1;
        for (n = CS.scopes[s].names; n; n = n->next)
            if (XS_EQ((const struct xs *)n->a, name)) return 1;
    }
    return 0;
}

/* Bound by a let, a lambda or a rec? A `with` does not count. */
static int is_lexically_bound(const struct xs *name)
{
    uint16_t s;
    const struct xc *n;
    for (s = CS.sbase; s < CS.nscope; ++s)
        for (n = CS.scopes[s].names; n; n = n->next)
            if (XS_EQ((const struct xs *)n->a, name)) return 1;
    return 0;
}

/* --- maps, files, positions ----------------------------------------------------------------- */
static int map_get(const struct xc *m, const uint8_t *k, uint16_t n, uint16_t *chunk)
{
    for (; m; m = m->next)
        if (((const struct xs *)m->a)->len == n && is((const struct xs *)m->a, k, n)) {
            *chunk = m->line;
            return 1;
        }
    return 0;
}

static void map_put(struct xc **m, const struct xs *key, uint16_t chunk)
{
    struct xc *c = cell((void *)key, 0, chunk, 0);
    c->next = *m;
    *m = c;
}

/* A source file's name in positions: Nix's own for the embedded
 * derivation.nix, else the fixture convention's -- FIXTURE_PWD and its path
 * from the compiled file's directory, or its name alone */
static const struct xs *file_name(const struct xs *key)
{
    const uint8_t *p = XS_TEXT(key);
    uint8_t *j = (uint8_t *)FRONT_SCRATCH;
    uint16_t k = key->len, b, d = CS.main_dir_n;
    if (IS(key, DERIVATION_KEY)) return XS_LIT("/derivation-internal.nix");
    if (CS.main_dir && k > d && memcmp(p, CS.main_dir, d) == 0 && p[d] == '/') {
        b = (uint16_t)(d + 1);                      /* strip_prefix */
    } else {
        for (b = k; b > 0 && p[b - 1] != '/'; --b) ;  /* file_name */
    }
    memcpy(j, FIXTURE_PWD "/", sizeof(FIXTURE_PWD));
    memcpy(j + sizeof(FIXTURE_PWD), p + b, (uint16_t)(k - b));
    return xs_of(j, (uint16_t)(sizeof(FIXTURE_PWD) + k - b));
}

static int16_t file_index(const struct xs *name)
{
    struct xc *f, **t = &CS.files;
    int16_t i = 0;
    for (f = CS.files; f; f = f->next, ++i)
        if (XS_EQ((const struct xs *)f->a, name)) return i;
    while (*t) t = &(*t)->next;
    *t = cell((void *)name, 0, 0, 0);
    return (int16_t)CS.nfiles++;
}

/* positions_for: consecutive positions for a set's attributes, in the order
 * given (the cells' line and col); the first's index, or 0 -- none --
 * outside a file or for a set Nix builds without positions */
static uint16_t positions_for(const struct xc *poss, uint16_t n)
{
    const struct xc *p;
    struct xc *c;
    uint16_t first, i;
    if (CS.cur_file < 0 || n == 0) return 0;
    for (p = poss, i = 0; i < n; p = p->next, ++i)
        if (p->line == 0) return 0;
    first = (uint16_t)(CS.npositions + 1);
    if ((uint32_t)first + n > 0xFFFFUL) refuse();   /* more than 65535 attribute positions */
    for (p = poss, i = 0; i < n; p = p->next, ++i) {
        c = cell(0, 0, p->line, p->col);
        c->a = (void *)(uint32_t)(uint16_t)CS.cur_file;
        if (CS.pos_tail) CS.pos_tail->next = c;
        else CS.positions = c;
        CS.pos_tail = c;
    }
    CS.npositions = (uint16_t)(CS.npositions + n);
    return first;
}

/* --- sorting by name bytes, stably (sort_by on as_bytes) ------------------------------------ */
static int bytes_lt(const struct xs *a, const struct xs *b)
{
    uint16_t n = a->len < b->len ? a->len : b->len;
    int c = n ? memcmp(XS_TEXT(a), XS_TEXT(b), n) : 0;
    return c < 0 || (c == 0 && a->len < b->len);
}

static struct xc *merge_sort(struct xc *l, uint16_t n)
{
    struct xc *r, *mid, *out = 0, **t = &out;
    uint16_t i, h = (uint16_t)(n >> 1);
    if (n < 2) return l;
    for (mid = l, i = 1; i < h; ++i) mid = mid->next;
    r = mid->next;
    mid->next = 0;
    l = merge_sort(l, h);
    r = merge_sort(r, (uint16_t)(n - h));
    while (l && r) {
        if (bytes_lt((const struct xs *)r->a, (const struct xs *)l->a)) { *t = r; r = r->next; }
        else { *t = l; l = l->next; }
        t = &(*t)->next;
    }
    *t = l ? l : r;
    return out;
}

/* A sorted copy of a list of cells whose a is a name */
static struct xc *sorted(const struct xc *in)
{
    struct xc *out = 0, **t = &out;
    uint16_t n = 0;
    for (; in; in = in->next, ++n) {
        *t = cell(in->a, in->b, in->line, in->col);
        t = &(*t)->next;
    }
    return merge_sort(out, n);
}

/* --- the compiler ------------------------------------------------------------------------------ */
static void strict(const struct xn *e, uint16_t ch);
static void lazy(const struct xn *e, uint16_t ch);
static void emit_builtin(const uint8_t *p, uint16_t n, uint16_t ch);
static void emit_builtins_set(uint16_t ch);
static void closed_thunk(const struct xs *path, const struct xn *e, uint16_t ch);

static void thunk_over(const struct xn *e, uint16_t ch, uint8_t op)
{
    uint16_t b = new_chunk(KIND_CODE);
    strict(e, b);
    emit_return(b);
    emit(ch, op);
    emit_u16(ch, b);
}

/* A one-argument lambda: its body's environment is the argument and the
 * closure's, a one-binding let's shape. With `args`, LAMBDA_ARGS, that
 * chunk building builtins.functionArgs' set. */
static void emit_lambda(const struct xs *param, const struct xn *body, uint16_t ch, uint16_t args,
                        uint8_t kind, const struct xs *info, uint8_t ellipsis)
{
    uint16_t b = new_chunk(kind);
    CH(b)->info = info;
    CH(b)->ellipsis = ellipsis;
    scope_push(cell((void *)param, 0, 0, 0), 0);
    strict(body, b);
    emit_return(b);
    --CS.nscope;
    if (args == NO_ARGS) {
        emit(ch, OP_LAMBDA);
        emit_u16(ch, b);
    } else {
        emit(ch, OP_LAMBDA_ARGS);
        emit_u16(ch, b);
        emit_u16(ch, args);
    }
}

/* One step of a selection: a name's constant, or a computed key, forced and
 * checked to be a string without context */
static void emit_key(const struct xc *key, uint16_t ch)
{
    if (key->a) {
        emit_str(ch, (const struct xs *)key->a);
    } else {
        strict((const struct xn *)key->b, ch);
        emit(ch, OP_ATTR_NAME);
    }
}

/* A run-time `with` lookup, the value left unforced; a miss falls back to
 * the global, or to `builtins` */
static void emit_with_get(const struct xs *name, const char *g, uint16_t ch)
{
    uint16_t miss, end;
    emit_str(ch, name);
    if (!g && !IS(name, "builtins")) {
        emit(ch, OP_WITH_GET);
        return;
    }
    emit(ch, OP_TRY_WITH_GET);
    miss = emit_jump(ch, OP_JMP_IF_NOTFOUND);
    end = emit_jump(ch, OP_JMP);
    patch_jump(ch, miss);
    emit(ch, OP_POP);
    if (g) emit_builtin((const uint8_t *)g, TLEN(g), ch);
    else emit_builtins_set(ch);
    patch_jump(ch, end);
}

/* A resolved name that is not a `with` lookup, unforced */
static void emit_ref(const struct binding *r, uint16_t ch)
{
    switch (r->kind) {
    case R_LITERAL: emit(ch, (uint8_t)r->depth); return;
    case R_GLOBAL: emit_builtin((const uint8_t *)r->global, TLEN(r->global), ch); return;
    case R_BUILTINS: emit_builtins_set(ch); return;
    }
    emit(ch, OP_ENV_GET);
    emit_u16(ch, r->depth);
    emit_u16(ch, r->index);
}

/* A name onto a list; b, a primop's table entry or 0 */
static struct xc *push_name(struct xc *l, const char *name, uint16_t n, const char *pr)
{
    struct xc *c = cell((void *)xs_of((const uint8_t *)name, n), (void *)pr, 0, 0);
    c->next = l;
    return c;
}

static void emit_primop(const char *pr, uint16_t n, uint16_t ch);

/* `builtins` as a value: one set a program, a closed thunk the VM memoises
 * by chunk; its `builtins` is that thunk again. Its names: the primops
 * that are Nix's builtins, the value builtins and `builtins`, sorted. */
static void emit_builtins_set(uint16_t ch)
{
    uint16_t b, n = 0, s_base, l;
    const char *t;
    struct xc *names = 0, *c;
    const struct xs *nm;
    if (!map_get(CS.imported, (const uint8_t *)BUILTINS_KEY, sizeof(BUILTINS_KEY) - 1, &b)) {
        b = new_chunk(KIND_CODE);
        map_put(&CS.imported, XS_LIT(BUILTINS_KEY), b);
        s_base = CS.sbase;
        CS.sbase = CS.nscope;
        for (t = PRIMOP_TABLE; (l = (uint8_t)*t) != 0; t += l + 4)
            if (nix_builtin((const uint8_t *)t + 1, l)) names = push_name(names, t + 1, l, t + 1);
        for (t = VALUE_TABLE; (l = (uint8_t)*t) != 0; t += (uint8_t)*t + 2) {
            names = push_name(names, t + 1, l, 0);
            t += l + 2;                             /* the value */
        }
        names = sorted(push_name(names, "builtins", 8, 0));
        for (c = names; c; c = c->next, ++n) {
            nm = (const struct xs *)c->a;
            emit_str(b, nm);
            if (c->b) {                             /* a primop: its entry in hand */
                emit_primop((const char *)c->b, nm->len, b);
            } else if (IS(nm, "builtins")) {
                emit(b, OP_CLOSED_THUNK);
                emit_u16(b, b);
            } else {
                emit_builtin(XS_TEXT(nm), nm->len, b);
            }
        }
        CS.sbase = s_base;
        emit(b, OP_ATTRS);
        emit_u16(b, n);
        emit_u16(b, 0);                             /* builtins: no positions */
        emit_return(b);
    }
    emit(ch, OP_CLOSED_THUNK);
    emit_u16(ch, b);
    emit(ch, OP_FORCE);
}

/* `derivation`: derivation.nix around derivationStrict, as in Nix --
 * embedded, lowered when a program first names it, compiled as an import
 * and forced. Nix embeds it after a newline, so its positions are a line on. */
static void emit_derivation(uint16_t ch)
{
    uint16_t b;
    struct xn *e;
    if (map_get(CS.imported, (const uint8_t *)DERIVATION_KEY, sizeof(DERIVATION_KEY) - 1, &b)) {
        emit(ch, OP_CLOSED_THUNK);
        emit_u16(ch, b);
    } else {
        e = lower_text((const uint8_t *)("\n" DERIVATION_NIX), (uint16_t)(DERIVATION_NIX_LEN + 1));
        closed_thunk(XS_LIT(DERIVATION_KEY), e, ch);
    }
    emit(ch, OP_FORCE);
}

/* A primop's lambda, one a program, curried an argument a chunk: pr is its
 * table entry, its name n bytes */
static void emit_primop(const char *pr, uint16_t n, uint16_t ch)
{
    uint16_t entry, inner, outer, d, arity = (uint8_t)pr[n + 2];
    uint8_t id = (uint8_t)pr[n + 1];
    if (!map_get(CS.builtins, (const uint8_t *)pr, n, &entry)) {
        inner = new_chunk(KIND_PRIMOP_APP);         /* every argument, outermost first */
        for (d = arity; d-- > 0; ) {
            emit(inner, OP_ENV_GET);
            emit_u16(inner, d);
            emit_u16(inner, 1);
        }
        emit(inner, OP_PRIM);
        emit(inner, id);
        emit_return(inner);
        for (d = 1; d < arity; ++d) {
            outer = new_chunk(KIND_PRIMOP_APP);
            emit(outer, OP_LAMBDA);
            emit_u16(outer, inner);
            emit_return(outer);
            inner = outer;
        }
        CH(inner)->kind = KIND_PRIMOP;              /* the one a name gives */
        map_put(&CS.builtins, xs_of((const uint8_t *)pr, n), inner);
        entry = inner;
    }
    emit(ch, OP_LAMBDA);
    emit_u16(ch, entry);
}

/* builtins.NAME with `builtins` unshadowed, or a global */
static void emit_builtin(const uint8_t *p, uint16_t n, uint16_t ch)
{
    const char *v, *pr;
    uint16_t d;
    uint8_t *num;
    if (n == 10 && memcmp(p, "derivation", 10) == 0) { emit_derivation(ch); return; }
    if ((v = builtin_value(p, n)) != 0) {
        if (*v == 'S') {
            emit_str(ch, xs_of((const uint8_t *)v + 1, (uint16_t)(TLEN(v) - 1)));
        } else if (*v == 'I') {
            num = (uint8_t *)zalloc(8);
            for (d = 0, ++v; *v; ++v) d = (uint16_t)(d * 10U + (uint16_t)(*v - '0'));
            num[0] = (uint8_t)d;
            num[1] = (uint8_t)(d >> 8);
            emit_const(ch, K_INT, num);
        } else {
            emit(ch, OP_LIST);
            emit_u16(ch, 0);
        }
        return;
    }
    if (n == 8 && memcmp(p, "builtins", 8) == 0) { emit_builtins_set(ch); return; }
    pr = nix_builtin(p, n) ? primop(p, n) : 0;
    if (!pr) {
        if (nix_builtin(p, n)) refuse();            /* builtins.NAME the VM lacks */
        emit_builtins_set(ch);                      /* no builtin at all: a missing attribute */
        emit_str(ch, xs_of(p, n));
        emit(ch, OP_SELECT);
        return;
    }
    emit_primop(pr, n, ch);
}

/* An imported file: its own chunk, one a file however many sites import
 * it, claimed before it is compiled into, with no enclosing scopes, reached
 * through a CLOSED_THUNK */
static void closed_thunk(const struct xs *path, const struct xn *e, uint16_t ch)
{
    uint16_t b, s_base;
    int16_t outer_file = CS.cur_file;
    if (!map_get(CS.imported, XS_TEXT(path), path->len, &b)) {
        b = new_chunk(KIND_CODE);
        map_put(&CS.imported, path, b);
        s_base = CS.sbase;
        CS.sbase = CS.nscope;
        if (outer_file >= 0) CS.cur_file = file_index(file_name(path));
        strict(e, b);
        CS.sbase = s_base;
        CS.cur_file = outer_file;
        emit_return(b);
    }
    emit(ch, OP_CLOSED_THUNK);
    emit_u16(ch, b);
}

static void strict_name(const struct xs *name, uint16_t skip, uint16_t ch)
{
    struct binding r;
    resolve_from(name, skip, &r);
    if (r.kind == R_WITH) {
        emit_with_get(name, r.global, ch);
        emit(ch, OP_FORCE);
        return;
    }
    emit_ref(&r, ch);
    if (r.kind == R_ENV) emit(ch, OP_FORCE);
}

static void binding(const struct xn *init, uint16_t idx, uint16_t ch);

static void strict_let(struct xc *binds, const struct xn *body, uint16_t ch)
{
    uint16_t n = count(binds), i;
    const struct xc *b;
    scope_push(binds, 0);
    emit(ch, OP_ENV_NEW);
    emit_u16(ch, n);
    for (i = 0, b = binds; b; b = b->next, ++i) {
        binding((const struct xn *)b->b, i, ch);
        emit(ch, OP_ENV_SET);
        emit_u16(ch, (uint16_t)(i + 1));
    }
    strict(body, ch);
    emit(ch, OP_ENV_POP);
    --CS.nscope;
}

/* { a, b ? d } @ s: body  ==  s: let a = s.a; b = s.b or d; in body, the
 * argument forced first; and a chunk building functionArgs' set */
static void strict_formals(const struct xn *e, uint16_t ch)
{
    const struct xs *arg = e->c ? (const struct xs *)e->c : XS_LIT("@args");
    struct xc *binds = 0, **t = &binds, *fs, *c;
    const struct xc *en;
    struct xn *v, *body;
    uint16_t args, n, p;
    for (en = (const struct xc *)e->a; en; en = en->next) {
        v = mk(X_VAR, (void *)arg, 0, 0);
        v->line = en->line;
        v->col = en->col;
        *t = cell(en->a, mk(X_SELECT, v, cell(en->a, 0, 0, 0), en->b), en->line, en->col);
        t = &(*t)->next;
    }
    v = mk(X_VAR, (void *)arg, 0, 0);
    v->line = e->line;
    v->col = e->col;
    body = mk(X_SEQ, v, mk(X_LET, binds, e->b, 0), 0);
    args = new_chunk(KIND_CODE);
    fs = sorted((const struct xc *)e->a);
    for (c = fs; c; c = c->next) {
        emit_str(args, (const struct xs *)c->a);
        emit(args, c->b ? OP_TRUE : OP_FALSE);
    }
    n = count(fs);
    p = positions_for(fs, n);                       /* functionArgs: the formals' positions */
    emit(args, OP_ATTRS);
    emit_u16(args, n);
    emit_u16(args, p);
    emit_return(args);
    emit_lambda(arg, body, ch, args, KIND_FORMALS, e->c ? (const struct xs *)e->c : XS_LIT(""),
                (uint8_t)((e->op & 2) != 0));
}

static void strict_attrs(const struct xn *e, uint16_t ch)
{
    struct xc *s = sorted((const struct xc *)e->a), *c;
    uint16_t n, p;
    for (c = s; c; c = c->next) {
        emit_str(ch, (const struct xs *)c->a);
        lazy((const struct xn *)c->b, ch);
    }
    n = count(s);
    p = positions_for(s, n);
    emit(ch, OP_ATTRS);
    emit_u16(ch, n);
    emit_u16(ch, p);
}

/* The static attributes as for ATTRS, then each computed name, forced, in
 * source order, and its value */
static void strict_dynattrs(const struct xn *e, uint16_t ch)
{
    struct xc *s = sorted((const struct xc *)e->a), *c;
    const struct xc *d, *dyns = (const struct xc *)e->b;
    uint16_t n, m, p, q;
    for (c = s; c; c = c->next) {
        emit_str(ch, (const struct xs *)c->a);
        lazy((const struct xn *)c->b, ch);
    }
    for (d = dyns; d; d = d->next) {
        strict((const struct xn *)d->a, ch);
        emit(ch, OP_FORCE);
        lazy((const struct xn *)d->b, ch);
    }
    n = count(s);
    m = count(dyns);
    p = positions_for(s, n);
    q = positions_for(dyns, m);
    emit(ch, OP_ATTRS_DYN);
    emit_u16(ch, n);
    emit_u16(ch, m);
    emit_u16(ch, p);
    emit_u16(ch, q);
}

/* rec { ... }  ==  let <bindings> in { inherit <names>; <computed> } */
static void strict_recset(const struct xn *e, uint16_t ch)
{
    struct xc *set = 0, **t = &set;
    const struct xc *b;
    struct xn *v;
    for (b = (const struct xc *)e->a; b; b = b->next) {
        v = mk(X_VAR, b->a, 0, 0);
        v->line = b->line;
        v->col = b->col;
        *t = cell(b->a, v, b->line, b->col);
        t = &(*t)->next;
    }
    strict_let((struct xc *)e->a, mk(e->b ? X_DYNATTRS : X_ATTRS, set, e->b, 0), ch);
}

static void strict_str(const struct xn *e, uint16_t ch)
{
    const struct xc *p = (const struct xc *)e->a;
    uint16_t n;
    if (!p) { emit_str(ch, XS_LIT("")); return; }
    if (!p->next && p->a) { emit_str(ch, (const struct xs *)p->a); return; }
    for (; p; p = p->next) {
        if (p->a) emit_str(ch, (const struct xs *)p->a);
        else strict((const struct xn *)p->b, ch);
    }
    n = count((const struct xc *)e->a);
    emit(ch, OP_INTERPOLATE);
    emit_u16(ch, n);
}

/* A path with interpolations: its parts as a list -- the VM forces and
 * coerces them in order, as Nix does -- for the internal pathInterp primop */
static void strict_pathi(const struct xn *e, uint16_t ch)
{
    const struct xc *p;
    struct xc *items = 0, **t = &items;
    for (p = (const struct xc *)e->a; p; p = p->next) {
        *t = cell(p->a ? (void *)mk(X_STR, cell(p->a, 0, 0, 0), 0, 0) : p->b, 0, 0, 0);
        t = &(*t)->next;
    }
    strict(mk(X_LIST, items, 0, 0), ch);
    emit(ch, OP_PRIM);
    emit(ch, PRIM_PATHINTERP);
}

/* builtins.NAME: base a `builtins` variable, one name, no default */
static const struct xs *builtins_dot(const struct xn *e)
{
    const struct xn *base = (const struct xn *)e->a;
    const struct xc *path = (const struct xc *)e->b;
    if (base->tag != X_VAR || !IS((const struct xs *)base->a, "builtins")) return 0;
    if (!path || path->next || !path->a || e->c) return 0;
    return (const struct xs *)path->a;
}

static void strict_select(const struct xn *e, uint16_t ch)
{
    const struct xc *k;
    const struct xs *name = builtins_dot(e), *bs;
    struct xc *misses = 0, **t = &misses, *m;
    uint16_t miss, end;
    if (name) {
        bs = XS_LIT("builtins");
        if (!is_shadowed(bs)) { emit_builtin(XS_TEXT(name), name->len, ch); return; }
        if (!is_lexically_bound(bs)) {             /* only a `with` stands in the way: ask it */
            emit_str(ch, bs);
            emit(ch, OP_TRY_WITH_GET);
            miss = emit_jump(ch, OP_JMP_IF_NOTFOUND);
            emit_str(ch, name);
            emit(ch, OP_SELECT);
            end = emit_jump(ch, OP_JMP);
            patch_jump(ch, miss);
            emit(ch, OP_POP);
            emit_builtin(XS_TEXT(name), name->len, ch);
            patch_jump(ch, end);
            return;
        }
    }
    strict((const struct xn *)e->a, ch);
    if (!e->c) {
        for (k = (const struct xc *)e->b; k; k = k->next) {
            emit_key(k, ch);
            emit(ch, OP_SELECT);
        }
        return;
    }
    for (k = (const struct xc *)e->b; k; k = k->next) {   /* any missing step takes the default */
        emit_key(k, ch);
        emit(ch, OP_TRY_SELECT);
        *t = cell(0, 0, emit_jump(ch, OP_JMP_IF_NOTFOUND), 0);
        t = &(*t)->next;
    }
    end = emit_jump(ch, OP_JMP);
    for (m = misses; m; m = m->next) patch_jump(ch, m->line);
    emit(ch, OP_POP);
    strict((const struct xn *)e->c, ch);
    patch_jump(ch, end);
}

static void strict_hasattr(const struct xn *e, uint16_t ch)
{
    const struct xc *k = (const struct xc *)e->b;
    struct xc *misses = 0, **t = &misses, *m;
    uint16_t end;
    strict((const struct xn *)e->a, ch);
    for (; k->next; k = k->next) {
        emit_key(k, ch);
        emit(ch, OP_TRY_SELECT);
        *t = cell(0, 0, emit_jump(ch, OP_JMP_IF_NOTFOUND), 0);
        t = &(*t)->next;
    }
    emit_key(k, ch);
    emit(ch, OP_HAS_ATTR);
    if (misses) {
        end = emit_jump(ch, OP_JMP);
        for (m = misses; m; m = m->next) patch_jump(ch, m->line);
        emit(ch, OP_POP);
        emit(ch, OP_FALSE);
        patch_jump(ch, end);
    }
}

static void strict_two(const struct xn *e, uint16_t ch)
{
    strict((const struct xn *)e->a, ch);
    strict((const struct xn *)e->b, ch);
}

/* a && b, a || b, a -> b (== !a || b) */
static void strict_logic(const struct xn *e, uint16_t ch)
{
    uint16_t j;
    strict((const struct xn *)e->a, ch);
    if (e->tag == X_IMPLIES) emit(ch, OP_NOT);
    j = emit_jump(ch, e->tag == X_AND ? OP_JMP_IF_FALSE : OP_JMP_IF_TRUE);
    emit(ch, OP_POP);
    strict((const struct xn *)e->b, ch);
    patch_jump(ch, j);
}

static void strict_if(const struct xn *e, uint16_t ch)
{
    uint16_t to_else, to_end;
    strict((const struct xn *)e->a, ch);
    to_else = emit_jump(ch, OP_JMP_IF_FALSE);
    emit(ch, OP_POP);
    strict((const struct xn *)e->b, ch);
    to_end = emit_jump(ch, OP_JMP);
    patch_jump(ch, to_else);
    emit(ch, OP_POP);
    strict((const struct xn *)e->c, ch);
    patch_jump(ch, to_end);
}

static void strict_list(const struct xn *e, uint16_t ch)
{
    const struct xc *it;
    uint16_t n;
    for (it = (const struct xc *)e->a; it; it = it->next) lazy((const struct xn *)it->a, ch);
    n = count((const struct xc *)e->a);
    emit(ch, OP_LIST);
    emit_u16(ch, n);
}

static uint8_t bin_op(uint8_t op)
{
    switch (op) {
    case B_ADD: return OP_ADD;
    case B_SUB: return OP_SUB;
    case B_MUL: return OP_MUL;
    case B_DIV: return OP_DIV;
    case B_EQ: case B_NE: return OP_EQ;
    case B_LT: return OP_LT;
    case B_LE: return OP_LE;
    case B_GT: return OP_GT;
    }
    return OP_GE;
}

/* strict: the expression's value on top of the stack. Its nesting is
 * recursion, ended by the C stack. */
static void strict(const struct xn *e, uint16_t ch)
{
    stack_check();
    switch (e->tag) {
    case X_INT: emit_const(ch, K_INT, &e->a); return;
    case X_FLOAT: emit_const(ch, K_FLOAT, &e->a); return;
    case X_PATH: emit_const(ch, K_PATH, e->a); return;
    case X_VAR: strict_name((const struct xs *)e->a, 0, ch); return;
    case X_INHERIT: strict_name((const struct xs *)e->a, 1, ch); return;
    case X_WITH:                                    /* the set forced only when a name needs it */
        lazy((const struct xn *)e->a, ch);
        emit(ch, OP_WITH_PUSH);
        scope_push(0, 1);
        strict((const struct xn *)e->b, ch);
        emit(ch, OP_ENV_POP);
        --CS.nscope;
        return;
    case X_NEG: strict((const struct xn *)e->a, ch); emit(ch, OP_NEG); return;
    case X_NOT: strict((const struct xn *)e->a, ch); emit(ch, OP_NOT); return;
    case X_BIN:
        strict_two(e, ch);
        emit(ch, bin_op(e->op));
        if (e->op == B_NE) emit(ch, OP_NOT);
        return;
    case X_AND: case X_OR: case X_IMPLIES: strict_logic(e, ch); return;
    case X_IF: strict_if(e, ch); return;
    case X_LET: strict_let((struct xc *)e->a, (const struct xn *)e->b, ch); return;
    case X_LAMBDA:
        if (e->op & 1) strict_formals(e, ch);
        else emit_lambda((const struct xs *)e->a, (const struct xn *)e->b, ch, NO_ARGS, KIND_LAMBDA,
                         (const struct xs *)e->a, 0);
        return;
    case X_SEQ:
        strict((const struct xn *)e->a, ch);
        emit(ch, OP_POP);
        strict((const struct xn *)e->b, ch);
        return;
    case X_IMPORT:                                  /* evaluated once, memoised in place */
        closed_thunk((const struct xs *)e->a, (const struct xn *)e->b, ch);
        emit(ch, OP_FORCE);
        return;
    case X_APPLY:                                   /* the argument lazy */
        strict((const struct xn *)e->a, ch);
        lazy((const struct xn *)e->b, ch);
        emit_call(ch);
        return;
    case X_ASSERT:
        strict((const struct xn *)e->a, ch);
        emit(ch, OP_ASSERT);
        strict((const struct xn *)e->b, ch);
        return;
    case X_STR: strict_str(e, ch); return;
    case X_LIST: strict_list(e, ch); return;
    case X_PATHI: strict_pathi(e, ch); return;
    case X_ATTRS: strict_attrs(e, ch); return;
    case X_RECSET: strict_recset(e, ch); return;
    case X_DYNATTRS: strict_dynattrs(e, ch); return;
    case X_SELECT: strict_select(e, ch); return;
    case X_HASATTR: strict_hasattr(e, ch); return;
    case X_UPDATE: strict_two(e, ch); emit(ch, OP_UPDATE); return;
    case X_CONCAT: strict_two(e, ch); emit(ch, OP_CONCAT); return;
    }
    refuse();
}

/* lazy: a literal directly, a variable as a pointer copy, a lambda or a
 * primop as the value it is, anything else a thunk in its own chunk */
static void lazy(const struct xn *e, uint16_t ch)
{
    struct binding r;
    const struct xc *p;
    const struct xs *name;
    switch (e->tag) {
    case X_INT: case X_PATH: case X_LAMBDA: strict(e, ch); return;
    case X_STR:
        p = (const struct xc *)e->a;
        if (!p || (!p->next && p->a)) { strict(e, ch); return; }
        break;
    case X_VAR:
        resolve_from((const struct xs *)e->a, 0, &r);
        if (r.kind != R_WITH) { emit_ref(&r, ch); return; }
        break;                                      /* a with-lookup forces its set */
    case X_SELECT:                                  /* directly only what cannot fail */
        name = builtins_dot(e);
        if (name && !is_shadowed(XS_LIT("builtins")) &&
            (primop(XS_TEXT(name), name->len) || builtin_value(XS_TEXT(name), name->len) ||
             IS(name, "derivation") || IS(name, "builtins"))) {
            emit_builtin(XS_TEXT(name), name->len, ch);
            return;
        }
        break;
    }
    thunk_over(e, ch, OP_THUNK);
}

/* A let binding's initialiser: as lazy, but a bare reference to a later
 * binding of the same let, or itself, through a thunk -- its slot is null
 * yet -- and `inherit x` from outside the let */
static void binding(const struct xn *init, uint16_t idx, uint16_t ch)
{
    struct binding r;
    if (init->tag == X_VAR) {
        resolve_from((const struct xs *)init->a, 0, &r);
        if (r.kind == R_ENV && r.depth == 0 && r.index > idx) thunk_over(init, ch, OP_THUNK);
        else lazy(init, ch);
        return;
    }
    if (init->tag == X_INHERIT) {
        resolve_from((const struct xs *)init->a, 1, &r);
        if (r.kind == R_WITH) thunk_over(init, ch, OP_THUNK);
        else emit_ref(&r, ch);
        return;
    }
    lazy(init, ch);
}

/* --- the image (nbc.rs write_program; BYTECODE.md "File layout") --------------------------- */
static void o8(uint8_t b)
{
    if ((uint32_t)CS.io >= IMAGE_BASE + IMAGE_MAX) nomem();
    *CS.io++ = b;
}
static void o16(uint16_t v)
{
    if ((uint32_t)CS.io + 2 > IMAGE_BASE + IMAGE_MAX) nomem();
    *(uint16_t *)CS.io = v;                         /* little-endian, as the image is */
    CS.io += 2;
}
static void o32(uint32_t v) { o16((uint16_t)v); o16((uint16_t)(v >> 16)); }
static void obytes(const uint8_t *b, uint16_t n)
{
    if (IMAGE_BASE + IMAGE_MAX - (uint32_t)CS.io < n) nomem();
    memcpy(CS.io, b, n);
    CS.io += n;
}

static uint32_t body_len(const struct chunk *c)
{
    const struct xc *k;
    uint32_t n = 4UL + c->len;
    for (k = c->consts; k; k = k->next)
        n += (k->line == K_INT || k->line == K_FLOAT) ? 9U : 3U + ((const struct xs *)k->a)->len;
    return n;
}

static void write_chunk(const struct chunk *c)
{
    const struct xc *k;
    const struct xs *s;
    uint16_t i, b = c->first;
    o16(c->nconsts);
    o16(c->len);
    for (k = c->consts; k; k = k->next) {
        o8((uint8_t)k->line);
        if (k->line == K_INT || k->line == K_FLOAT) {
            obytes((const uint8_t *)k->a, 8);
        } else {
            s = (const struct xs *)k->a;
            o16(s->len);
            obytes(XS_TEXT(s), s->len);
        }
    }
    for (i = c->len; i > BLK_DATA; i -= BLK_DATA) {   /* a block at a time */
        obytes(BLOCK(b) + 2, BLK_DATA);
        b = NEXT(b);
    }
    obytes(BLOCK(b) + 2, i);
}

/* The header, the chunk table, the chunks; then, a chunk not plain code,
 * the kinds, their info records' offsets and the records; then, a
 * position recorded, the files and the positions. (No 32-bit product: it
 * would link ~UMUL4 into bank 0; CHUNKS_N keeps the chunks' in 16 bits.) */
static uint16_t write_program(void)
{
    const struct chunk *c;
    const struct xc *f, *p;
    uint16_t table = (uint16_t)(20U + CS.nchunk * 4U);
    uint32_t body = table, info_at, infos = 0, before, tbl = 0, off;
    uint16_t i;
    int kinded = 0;
    for (i = 0; i < CS.nchunk; ++i) {
        c = CH(i);
        body += body_len(c);
        if (c->kind != KIND_CODE) kinded = 1;
        if (c->info) infos += 3U + c->info->len;
    }
    if (CS.npositions) {
        tbl = 4;
        for (f = CS.files; f; f = f->next) tbl += 2U + ((const struct xs *)f->a)->len;
        for (p = CS.positions; p; p = p->next) tbl += 6;
    }
    info_at = body + (uint16_t)(CS.nchunk * 3U);
    before = kinded ? info_at + infos : body;
    CS.io = (uint8_t *)IMAGE_BASE;
    o8('N');
    o8('B');
    o8('C');
    o8(2);                                          /* the version */
    o16(CS.nchunk);
    o16(0);                                         /* the entry chunk */
    o32(before + tbl);
    o32(kinded ? body : 0);
    o32(tbl ? before : 0);
    off = table;
    for (i = 0; i < CS.nchunk; ++i) {
        o32(off);
        off += body_len(CH(i));
    }
    for (i = 0; i < CS.nchunk; ++i) write_chunk(CH(i));
    if (kinded) {
        for (i = 0; i < CS.nchunk; ++i) o8(CH(i)->kind);
        off = info_at;
        for (i = 0; i < CS.nchunk; ++i) {
            c = CH(i);
            o16(c->info ? (uint16_t)off : 0);
            if (c->info) off += 3U + c->info->len;
        }
        for (i = 0; i < CS.nchunk; ++i) {
            c = CH(i);
            if (!c->info) continue;
            o8(c->ellipsis);
            o16(c->info->len);
            obytes(XS_TEXT(c->info), c->info->len);
        }
    }
    if (tbl) {
        o16(CS.nfiles);
        for (f = CS.files; f; f = f->next) {
            o16(((const struct xs *)f->a)->len);
            obytes(XS_TEXT((const struct xs *)f->a), ((const struct xs *)f->a)->len);
        }
        o16(CS.npositions);
        for (p = CS.positions; p; p = p->next) {
            o16((uint16_t)(uint32_t)p->a);
            o16(p->line);
            o16(p->col);
        }
    }
    return (uint16_t)((uint32_t)CS.io - IMAGE_BASE);
}

/* --- the whole file -------------------------------------------------------------------------- */
uint16_t compile_file(const struct xn *program)
{
    const uint8_t *name = (const uint8_t *)SRC_NAME;
    uint16_t n = cslen((const char *)name), dn;
    memset(&CS, 0, sizeof(struct cstate));
    CS.chunks = (struct chunk *)zalloc((uint16_t)(CHUNKS_N * sizeof(struct chunk)));
    CS.scopes = (struct scope *)zalloc((uint16_t)(SCOPES_N * sizeof(struct scope)));
    CS.cur_file = -1;
    for (dn = n; dn > 0 && name[dn - 1] != '/'; --dn) ;
    if (dn) {                                       /* positions in its sets, by its name */
        CS.main_dir = name;
        CS.main_dir_n = (uint16_t)(dn - 1);
        CS.cur_file = file_index(file_name(xs_of(name, n)));
    }
    new_chunk(KIND_CODE);                           /* chunk 0, the entry */
    strict(program, 0);
    emit_return(0);
    return write_program();
}

/* --compile (a2min): the source at SRC_BASE parsed, lowered and compiled,
 * the image at IMAGE_BASE and its length at RESULT_ADDR */
void compile_dump(void)
{
    uint16_t n = *(volatile uint16_t *)SRC_LEN_ADDR;
    struct xn *x;
    parse_file((const uint8_t *)SRC_BASE, n);
    x = lower_file((const uint8_t *)SRC_BASE, n);
    *(uint16_t *)RESULT_ADDR = compile_file(x);
}
