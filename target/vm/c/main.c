/* main.c -- entry, the loader, error exit and result reporting.
 *
 * vm_entry is the first function in the first file of the link, so it is the
 * load file's entry point: omf2bin's stub JSLs here in native mode with the
 * stack at STACK_TOP, and RTL returns to a2min's halt. No ORCALib start-up, no OS.
 */
#pragma noroot
#pragma optimize 15

#include "vm.h"

struct catch_frame catches[CATCH_N];
uint16_t catch_n;
obj_t *ctab_pool[CTAB_N];
uint16_t cnext;
struct chunk_info chunk_info[CHUNK_MAX];
obj_t *imports[CHUNK_MAX];
uint16_t nchunks;

/* The shared singletons. Immutable: FORCE never touches a value, and nothing
 * else writes through a pointer it did not just allocate. They live in this
 * program (bank 0); their pointers carry the RAM bit like young ones. */
static struct { obj_t h; uint8_t v[4]; } true_s  = { { T_BOOL, 0, 0, 0 }, { 1, 0, 0, 0 } };
static struct { obj_t h; uint8_t v[4]; } false_s = { { T_BOOL, 0, 0, 0 }, { 0, 0, 0, 0 } };
static struct { obj_t h; uint8_t v[4]; } null_s  = { { T_NULL, 0, 0, 0 }, { 0, 0, 0, 0 } };
static struct { obj_t h; char s[12]; obj_t *ctx; } functor_s = { { T_STRING, 9, 0, 0 }, "__functor", 0 };
static struct { obj_t h; char s[8];  obj_t *ctx; } success_s = { { T_STRING, 7, 0, 0 }, "success", 0 };
static struct { obj_t h; char s[8];  obj_t *ctx; } value_s   = { { T_STRING, 5, 0, 0 }, "value", 0 };

obj_t *obj_true, *obj_false, *obj_null, *str_functor, *str_success, *str_value;

/* The chunk make_apply's thunks run: (f x) from environment slots 1 and 2. */
static const uint8_t apply_code[12] = {
    OP_ENV_GET, 0, 0, 1, 0,
    OP_ENV_GET, 0, 0, 2, 0,
    OP_CALL,
    OP_RETURN
};

static jmp_buf top_jb;

static void load(void);
static void diag_write(void);

/* --- entry ------------------------------------------------------------------------------ */
/* FIRST in this file, and this file first in the link: the load file's entry
 * point is the first function of the first object, and the first cut had
 * err_exit here -- a2min entered it with garbage and longjmp'd through an
 * unset buffer. */
void vm_entry(void)
{
    uint16_t i;
    obj_true    = STATIC_PTR(&true_s);
    obj_false   = STATIC_PTR(&false_s);
    obj_null    = STATIC_PTR(&null_s);
    str_functor = STATIC_PTR(&functor_s);
    str_success = STATIC_PTR(&success_s);
    str_value   = STATIC_PTR(&value_s);
    for (i = 0; i < STATS_N; ++i) STAT(i) = 0;
    vsp = rsp = fstack_n = catch_n = rs_n = 0;
    gc_tick = 0;
    gc_period = *(volatile uint8_t *)GC_STRESS_ADDR;
    env = 0;
    gc_water = nfree = 0x0010;                  /* offset 0 stays unallocated */
    prims_init();
    card_find();
    memcpy((void *)(IMAGE_BASE + APPLY_CODE_OFF), apply_code, sizeof apply_code);
    if (setjmp(top_jb) == 0) {
        load();                                 /* its constants are roots, promoted like anything */
        run();                                  /* the entry chunk; RETURN brings us back */
        deep_force_top();
        *(obj_t **)RESULT_ADDR = pop();
        *(volatile uint8_t *)RESULT_STAT_ADDR = 1;
    }
    diag_write();
}


/* --- errors ------------------------------------------------------------------------- */
/* A catchable error -- throw or a failed assert, the two Nix lets tryEval
 * catch -- with a live catch frame unwinds to it (BYTECODE.md "Catch frames"):
 * every register the frame saved is restored, and every thunk that was under
 * evaluation inside the tryEval is made a THUNK again, so forcing one a second
 * time re-runs it (and re-throws), as Nix would. Anything else ends the run. */
_Noreturn void err_exit(uint8_t code)
{
    if ((code == E_THROW || code == E_ASSERT) && catch_n) {
        struct catch_frame *c = &catches[--catch_n];
        vsp = c->vsp;
        env = c->env;
        pc = c->pc;
        ctab = c->ctab;
        rsp = c->rsp;
        while (fstack_n > c->fstack_n) { obj_t *t = fstack[--fstack_n]; store_byte(t, 0, T_THUNK); }
        longjmp(c->jb, 1);
    }
    *(volatile uint8_t *)RESULT_ERR_ADDR = code;
    *(volatile uint8_t *)RESULT_STAT_ADDR = 0xFF;
    longjmp(top_jb, 1);
}

_Noreturn void nomem(void)    { err_exit(E_NOMEM); }
_Noreturn void type_err(void) { err_exit(E_TYPE); }
_Noreturn void badnbc(void)   { err_exit(E_BADNBC); }

/* --- the loader ----------------------------------------------------------------------- */
/* Validate the header, materialise every chunk's constants into the nursery,
 * and record each chunk's code and constant-table offsets. */
static uint16_t rd16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }

/* Constants live in the image bank, above the image, for good (VM.md
 * "Speed"): immutable and pointer-free, they need no collector -- and in the
 * nursery the first collection promoted them, after which every use of one
 * was a mature view. STATIC_PTR gives them the RAM bit and not the nursery's
 * bank ($8003, not $8001), so deref takes them as they are and the collector
 * leaves them where they are, as it does the singletons. When the bank is
 * full, the nursery, as before. */
static uint16_t konst;                          /* the next free offset in the image bank */

static obj_t *const_alloc(uint16_t size)
{
    obj_t *o;
    if ((uint16_t)(APPLY_CODE_OFF - konst) < size) return alloc(size);
    o = STATIC_PTR(IMAGE_BASE + konst);
    konst += size;
    return o;
}

static void load(void)
{
    const uint8_t *img = (const uint8_t *)IMAGE_BASE;
    const uint8_t *q;
    uint16_t ci, k, cnt, len, entry;
    uint32_t total, off;
    obj_t *o;
    if (img[0] != 'N' || img[1] != 'B' || img[2] != 'C' || img[3] != 2) badnbc();
    nchunks = rd16(img + 4);
    entry = rd16(img + 6);
    total = (uint32_t)rd16(img + 8) | ((uint32_t)rd16(img + 10) << 16);
    if (total > IMAGE_MAX || nchunks == 0 || nchunks > CHUNK_MAX) badnbc();
    cnext = 0;
    konst = round4((uint16_t)total);            /* the constants after the image */
    memset(imports, 0, sizeof imports);      /* no file imported yet */
    for (ci = 0; ci < nchunks; ++ci) {
        off = (uint32_t)rd16(img + 20 + 4 * ci) | ((uint32_t)rd16(img + 22 + 4 * ci) << 16);
        if (off >= total) badnbc();
        q = img + (uint16_t)off;
        cnt = rd16(q);
        q += 4;
        chunk_info[ci].ctab = cnext;
        for (k = 0; k < cnt; ++k) {
            if (cnext >= CTAB_N) nomem();
            switch (q[0]) {
            case K_INT:
            case K_FLOAT:                       /* the same shape: kind is tag */
                o = const_alloc(12);
                SET_HDR(o, q[0], 0);
                memcpy(PAYLOAD(o), q + 1, 8);
                q += 9;
                break;
            case K_STRING:                      /* str_alloc's shape: padded, then no context */
                len = rd16(q + 1);
                if (round4(len) < len) nomem();
                o = const_alloc(round4(len) + 8);
                SET_HDR(o, T_STRING, len);
                if (round4(len)) *(uint32_t *)(PAYLOAD(o) + round4(len) - 4) = 0;
                STRCTX(o, len) = 0;
                memcpy(STRBYTES(o), q + 3, len);
                q += 3 + len;
                break;
            case K_PATH:                        /* path_alloc's: padded, no context word */
                len = rd16(q + 1);              /* already canonical: the generator did it */
                if (round4(len) < len) nomem();
                o = const_alloc(round4(len) ? round4(len) + 4 : 8);
                SET_HDR(o, T_PATH, len);
                if (round4(len)) *(uint32_t *)(PAYLOAD(o) + round4(len) - 4) = 0;
                memcpy(STRBYTES(o), q + 3, len);
                q += 3 + len;
                break;
            default:
                badnbc();
            }
            ctab_pool[cnext++] = o;
        }
        chunk_info[ci].code = LOW16(q);
    }
    if (entry >= nchunks) badnbc();
    pc = IMAGE_PC(chunk_info[entry].code);
    ctab = chunk_info[entry].ctab;
}

/* --- the state on error, for a2min --run-nbc-c --------------------------------------- */
static void diag_write(void)
{
    uint16_t *d = (uint16_t *)DIAG_ADDR;
    d[0] = PC_OFF();
    d[1] = vsp;
    d[2] = rsp;
    d[3] = nfree;
    d[4] = fstack_n;
    d[5] = catch_n;
    *(uint32_t *)(d + 6) = (uint32_t)vstack;
}
