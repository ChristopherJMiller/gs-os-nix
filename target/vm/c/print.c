/* print.c -- the result as `nix-instantiate --eval --strict` prints it, on
 * the target (docs/COMPILER.md "Stage 4"): a2min's printer, which every
 * .exp has been held to, written to standard output through GS/OS. a2min
 * compares the two, byte for byte.
 *
 * It runs once the VM is done -- after the result is forced, or an error --
 * and runs no bytecode and allocates nothing, so nothing moves under it. It
 * takes no views: every object is read where it lives, in place or across
 * the bus (card_read_at), so the window ring is never involved. It is
 * iterative, its stack of open lists and sets in bank 12 (the compiler's,
 * free at run time), so nesting costs no C stack; what it has printed is a
 * table in bank 7 (a primop's scratch, free once the run is over). It never
 * fails: whatever it cannot print, it says.
 */
#pragma noroot
#pragma optimize 15

#include "vm.h"
#include "front.h"
#include "opcodes.h"
#include "nbc_tables.h"

segment "print";

#define DEPTH_MAX   1000                    /* a2min's printer's too: <DEEP> past it */
#define COUNT_MAX   0x4000UL                /* a list's or set's: past it, not one (<OOB>) */
#define SEEN        ((obj_t **)0x00070000UL) /* bank 7: the lists and sets printed */
#define SEEN_N      8192U                   /* 32 KB, cleared each time */
#define SEEN_FULL   6144U                   /* three quarters: probes stay short */
#define FRAMES      ((struct frame *)FRONT_SCRATCH)   /* bank 12: the open lists and sets */
#define OUT         ((uint8_t *)FRONT_CANON)          /* bank 12: the text, flushed as it fills */
#define OUT_N       0x2000U

struct frame { obj_t *p; uint16_t i, n, depth; uint8_t tag, pad; };

/* The printer's state, in bank 12's top with the compiler's (front.h
 * FRONT_CSTATE): no static of its own, so bank 0 pays nothing */
struct pstate { uint16_t out, ref, nseen, nframes; uint8_t full; };
#define PS (*(struct pstate *)(FRONT_CSTATE + 0x80UL))

struct gs_std { uint16_t pcount, prefix, ref; };
struct gs_io  { uint16_t pcount, ref; void *buf; uint32_t req, got; };

/* --- the text --------------------------------------------------------------------------- */
static void flush(void)
{
    struct gs_io w;
    if (PS.out == 0) return;
    w.pcount = 4;
    w.ref = PS.ref;
    w.buf = OUT;
    w.req = PS.out;
    w.got = 0;
    gsos_call(0x2013, &w);                        /* Write; an error has nowhere to be said */
    PS.out = 0;
}

static void put(const char *s, uint16_t n)
{
    while (n--) {
        if (PS.out == OUT_N) flush();
        OUT[PS.out++] = (uint8_t)*s++;
    }
}

static void puts_(const char *s)
{
    uint16_t n = 0;
    while (s[n]) ++n;
    put(s, n);
}

/* --- the heap, read where it lives ---------------------------------------------------------- */
/* n bytes of the object p (an original) from offset off */
static void rd(obj_t *p, uint16_t off, void *dst, uint16_t n)
{
    if (IS_MATURE(p)) card_read_at(PBITS(p) + off, dst, n);
    else memcpy(dst, (const uint8_t *)p + off, n);
}

static obj_t *rd_ptr(obj_t *p, uint16_t off)
{
    obj_t *q;
    rd(p, off, &q, 4);
    return q;
}

/* the header's tag, and its count (the 22 bits below the marks) */
static uint8_t head(obj_t *p, uint32_t *aux)
{
    obj_t h;
    rd(p, 0, &h, 4);
    *aux = (uint32_t)h.aux0 | ((uint32_t)h.aux1 << 8) | ((uint32_t)(h.aux2 & 0x3F) << 16);
    return h.tag;
}

/* --- what has been printed (a2min's seen set) ------------------------------------------------ */
/* 1 if p was printed before; else it is now. Linear probing over bank 7. */
static int seen(obj_t *p)
{
    uint16_t h = (uint16_t)((LOW16(p) >> 2) ^ HI16(p)) & (SEEN_N - 1);
    while (SEEN[h]) {
        if (SEEN[h] == p) return 1;
        h = (uint16_t)((h + 1) & (SEEN_N - 1));
    }
    if (PS.nseen == SEEN_FULL) { PS.full = 1; return 0; }
    SEEN[h] = p;
    ++PS.nseen;
    return 0;
}

/* --- scalars ---------------------------------------------------------------------------------- */
static void put_int(obj_t *p)
{
    int64_t v;
    uint64_t u;
    char buf[24], out[24];
    uint16_t n = 0, i;
    rd(p, 4, &v, 8);
    u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    if (u <= 0xFFFFU) {                         /* json.c's int_text: the narrowest division */
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
    put(out, n);
}

static void put_float(obj_t *p)
{
    uint64_t bits;
    char buf[F_JSON_MAX + 8];                   /* %g's 13, with room */
    rd(p, 4, &bits, 8);
    put(buf, f_g(bits, buf));                   /* ostream << double: %g */
}

/* A STRING's or PATH's bytes, quoted as Nix quotes a string or raw */
static void put_text(obj_t *p, uint32_t len, int quote)
{
    uint8_t buf[64], next;
    uint16_t at, k, n;
    char c;
    if (len > 0x10000UL) { puts_("<OOB>"); return; }
    if (quote) put("\"", 1);
    for (at = 0; at < len; at = (uint16_t)(at + n)) {
        n = (uint16_t)(len - at) < 64 ? (uint16_t)(len - at) : 64;
        rd(p, (uint16_t)(4 + at), buf, n);
        for (k = 0; k < n; ++k) {
            c = (char)buf[k];
            if (!quote) { put(&c, 1); continue; }
            switch (c) {
            case '"': put("\\\"", 2); break;
            case '\\': put("\\\\", 2); break;
            case '\n': put("\\n", 2); break;
            case '\r': put("\\r", 2); break;
            case '\t': put("\\t", 2); break;
            case '$':                           /* "${" is escaped; the next byte may be the next block's */
                if (k + 1 < n) next = buf[k + 1];
                else if ((uint32_t)at + k + 1 < len) rd(p, (uint16_t)(4 + at + k + 1), &next, 1);
                else next = 0;
                put(next == '{' ? "\\$" : "$", next == '{' ? 2 : 1);
                break;
            default: put(&c, 1);
            }
        }
    }
    if (quote) put("\"", 1);
}

/* What Nix's printer leaves unquoted: an identifier that is not a keyword
 * (`or` is contextual, not one). The keywords, each its length and bytes. */
#define KEYWORDS "\002if\004then\004else\006assert\004with\003let\002in\003rec\007inherit"

static int identifier(obj_t *name, uint32_t len)
{
    uint8_t buf[8], c;
    uint16_t i, n = (uint16_t)len;
    const char *k;
    if (len == 0 || len > 0xFFFFUL) return 0;
    for (i = 0; i < n; ++i) {
        rd(name, (uint16_t)(4 + i), &c, 1);
        if (i < 8) buf[i] = c;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') continue;
        if (i > 0 && ((c >= '0' && c <= '9') || c == '\'' || c == '-')) continue;
        return 0;
    }
    for (k = KEYWORDS; *k; k += (uint8_t)*k + 1)
        if ((uint8_t)*k == n && memcmp(k + 1, buf, n) == 0) return 0;
    return 1;
}

/* A chunk's kind (BYTECODE.md "Chunk kinds"), from the image's kinds; 0 if
 * the image has none */
static uint8_t chunk_kind(uint16_t i)
{
    const uint8_t *img = (const uint8_t *)IMAGE_BASE;
    uint16_t kinds = (uint16_t)(img[12] | ((uint16_t)img[13] << 8));    /* the image is < 64 KB */
    return kinds ? img[kinds + i] : 0;
}

/* An error code's name (opcodes.h, through nbc_tables.h), as a2min's */
static const char *error_name(uint8_t code)
{
    const char *t = ERROR_TABLE;
    uint8_t i;
    for (i = 0; i < code && *t; ++i) t += (uint8_t)*t + 2;
    return *t ? t + 1 : "E_?";
}

/* --- values ----------------------------------------------------------------------------------- */
/* Prints p, or opens it: a non-empty list or set not printed before is
 * pushed for the loop to print its elements */
static void value(obj_t *p, uint16_t depth)
{
    uint32_t aux;
    uint8_t tag;
    uint16_t code, i;
    struct frame *f;
    char hex[8];
    for (;;) {
        if (depth > DEPTH_MAX) { puts_("<DEEP>"); return; }
        if (p == 0) { puts_("<NULLPTR>"); return; }
        tag = head(p, &aux);
        if (tag != T_INDIRECT) break;
        p = rd_ptr(p, 4);                       /* a2min follows it a level deeper */
        ++depth;
    }
    switch (tag) {
    case T_INT: put_int(p); return;
    case T_FLOAT: put_float(p); return;
    case T_BOOL: { uint8_t b; rd(p, 4, &b, 1); puts_(b ? "true" : "false"); return; }
    case T_NULL: puts_("null"); return;
    case T_STRING: put_text(p, aux, 1); return;
    case T_PATH: put_text(p, aux, 0); return;
    case T_LIST: case T_ATTRS:
        if (aux > COUNT_MAX) { puts_("<OOB>"); return; }
        if (aux == 0) { puts_(tag == T_LIST ? "[ ]" : "{ }"); return; }
        if (seen(p)) { puts_("\302\253repeated\302\273"); return; }   /* «repeated» */
        if (PS.nframes == DEPTH_MAX + 2) { puts_("<DEEP>"); return; }
        put(tag == T_LIST ? "[" : "{", 1);
        f = &FRAMES[PS.nframes++];
        f->p = p;
        f->i = 0;
        f->n = (uint16_t)aux;
        f->depth = depth;
        f->tag = tag;
        return;
    case T_THUNK: puts_("<CODE>"); return;
    case T_BLACKHOLE: puts_("<CYCLE>"); return;
    case T_LAMBDA:                              /* a primop's chunk, by its kind (BYTECODE.md) */
        rd(p, 4, &code, 2);
        for (i = 0; i < nchunks; ++i)
            if (chunk_info[i].code == code) {
                switch (chunk_kind(i)) {
                case 1: puts_("<PRIMOP>"); return;
                case 2: puts_("<PRIMOP-APP>"); return;
                }
                break;
            }
        puts_("<LAMBDA>");
        return;
    }
    hex[0] = '<';
    hex[1] = '?';
    hex[2] = "0123456789ABCDEF"[tag >> 4];
    hex[3] = "0123456789ABCDEF"[tag & 15];
    hex[4] = '>';
    put(hex, 5);
}

/* The elements of the open lists and sets, innermost first. A frame's index
 * moves on before its element is printed, which may open a frame above it;
 * a set's `;` is printed when the frame is next come back to. */
static void drain(void)
{
    struct frame *f;
    obj_t *name;
    uint32_t len;
    uint16_t i;
    while (PS.nframes) {
        f = &FRAMES[PS.nframes - 1];
        if (f->tag == T_ATTRS && f->i) put(";", 1);   /* after the value just printed */
        if (f->i == f->n) {
            put(f->tag == T_LIST ? " ]" : " }", 2);
            --PS.nframes;
            continue;
        }
        i = f->i++;
        put(" ", 1);
        if (f->tag == T_LIST) {
            value(rd_ptr(f->p, (uint16_t)(4 + 4 * i)), (uint16_t)(f->depth + 1));
            continue;
        }
        name = rd_ptr(f->p, (uint16_t)(4 + 8 * f->n + 4 * i));
        if (!name) puts_("\"<NULLPTR>\"");      /* a2min quotes what it prints in its place */
        else if (head(name, &len), identifier(name, len)) put_text(name, len, 0);
        else put_text(name, len, 1);
        put(" = ", 3);
        value(rd_ptr(f->p, (uint16_t)(4 + 4 * f->n + 4 * i)), (uint16_t)(f->depth + 1));
    }
}

/* --- the run's answer -------------------------------------------------------------------------- */
/* What a2min prints: the value, or `error: E_NAME`, and for a throw, an
 * abort or a builtin that needs GS/OS its message (in RESULT); a line. Only
 * if a2min asks: vm_entry calls this every run, and the test is here, where
 * it costs bank 0 nothing. */
void print_result(void)
{
    struct gs_std s;
    uint8_t code;
    if (!*(volatile uint8_t *)PRINT_ADDR) return;
    memset(&PS, 0, sizeof(struct pstate));
    memset(SEEN, 0, (uint16_t)(SEEN_N * 4U));
    s.pcount = 2;
    s.prefix = 11;                              /* standard output */
    s.ref = 0;
    gsos_call(0x2037, &s);                        /* GetStdRefNum */
    PS.ref = s.ref;
    if (*(volatile uint8_t *)RESULT_STAT_ADDR == 0xFF) {
        code = *(volatile uint8_t *)RESULT_ERR_ADDR;
        puts_("error: ");
        puts_(error_name(code));
        if (code == E_THROW || code == E_ABORT || code == E_GSOS) {
            put(" ", 1);
            value(*(obj_t **)RESULT_ADDR, 0);
            drain();
        }
    } else {
        value(*(obj_t **)RESULT_ADDR, 0);
        drain();
    }
    if (PS.full) puts_(" <TOO MANY TO PRINT>");
    put("\n", 1);
    flush();
}
