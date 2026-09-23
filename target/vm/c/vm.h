/* vm.h -- the Nix bytecode VM in C (docs/VM.md). Compiled by ORCA/C, small
 * memory model: code and these globals share bank 0; the nursery is bank 1,
 * the collector's staging bank 2, the .nbc image bank 3.
 *
 * Object layouts are HEAP.md's; the bytecode is BYTECODE.md's; it was
 * translated routine for routine from the assembly VM (git: commit 05ce616).
 *
 * The one rule that matters: anything that runs bytecode -- run(), force_*,
 * do_call(), values_equal(), deep_force_top() -- may collect, and a collection
 * moves young objects. A C local of type obj_t * must not be held across such a
 * call; keep it on the root stack (rpush/rpop) and the collector rewrites it.
 * Allocation itself never collects: gc_maybe() runs only at run()'s dispatch.
 */
#ifndef VM_H
#define VM_H

#include <stdint.h>
#include <string.h>
#include <setjmp.h>
#include "opcodes.h"

/* --- the memory map (docs/VM.md) ------------------------------------------------ */
#define RESULT_ADDR      0x0300UL   /* 4-byte pointer to the result object */
#define DIAG_ADDR        0x0310UL   /* the VM's state on error, see diag_write() */
#define RESULT_STAT_ADDR 0x0320UL   /* $01 done, $FF error */
#define RESULT_ERR_ADDR  0x0321UL
#define GC_STRESS_ADDR   0x0BF0UL   /* a2min sets it: collect at every boundary */
#define STACK_MIN        0x0500U    /* the C stack (S) must stay above this */

#define YOUNG_TAG        0x80000000UL
#define NURSERY_BANK     0x01
#define NURSERY_BASE     0x00010000UL
#define NURSERY_END      0xFFF0U    /* offsets below this are the nursery */
#define STAGE_BASE       0x00020000UL
#define IMAGE_BASE       0x00030000UL
#define IMAGE_MAX        0xFFF0UL   /* the image must fit below APPLY_CODE */
#define APPLY_CODE_OFF   0xFFF0U    /* main.c copies apply_code[] there at start */
#define WINDOW_BASE      0x00050000UL   /* bank 5: copies of mature objects (data.c) */
#define WINDOW_RING      0xF000U    /* the ring of copies; an object larger than half of it is E_NOMEM */

#define GC_MARGIN        0x0800U    /* collect when less than this is free */

/* --- heap tags (HEAP.md) --------------------------------------------------------- */
#define T_INT        0x01
#define T_BOOL       0x02
#define T_NULL       0x03
#define T_STRING     0x04
#define T_PATH       0x05
#define T_LIST       0x06
#define T_ATTRS      0x07
#define T_THUNK      0x08
#define T_LAMBDA     0x09
#define T_INDIRECT   0x0A
#define T_BLACKHOLE  0x0B
#define T_FORWARDED  0x0C

/* aux2 marks: bit 7 = visited (deep force), bit 6 = a `with` environment */
#define AUX_MARK     0x80
#define AUX_WITH     0x40

typedef struct obj {
    uint8_t tag;
    uint8_t aux0, aux1, aux2;   /* 24-bit little-endian count/length, marks above */
} obj_t;

#define PBITS(p)     ((uint32_t)(p))
#define IS_YOUNG(p)  ((uint16_t)(PBITS(p) >> 16) == 0x8001U)   /* in the nursery */
#define IS_STATIC(p) ((uint16_t)(PBITS(p) >> 16) == 0x8000U)   /* in this program */
#define IS_RAM(p)    ((PBITS(p) & YOUNG_TAG) != 0)               /* dereferenceable */
#define IS_MATURE(p) ((p) != 0 && !IS_RAM(p))                     /* on the card */
#define IS_WINDOW(p) ((uint16_t)(PBITS(p) >> 16) == 0x8005U)   /* a copy of a mature object */
#define LOW16(p)     ((uint16_t)PBITS(p))
#define YOUNG_PTR(off) ((obj_t *)(YOUNG_TAG | NURSERY_BASE | (uint32_t)(uint16_t)(off)))
#define STATIC_PTR(o)  ((obj_t *)(YOUNG_TAG | (uint32_t)(o)))

#define AUX16(o)     ((uint16_t)((o)->aux0 | ((uint16_t)(o)->aux1 << 8)))
#define SET_HDR(o, t, n) ((o)->tag = (t), (o)->aux0 = (uint8_t)(n), \
                          (o)->aux1 = (uint8_t)((uint16_t)(n) >> 8), (o)->aux2 = 0)
#define PAYLOAD(o)   ((uint8_t *)(o) + 4)
#define INTVAL(o)    (*(int64_t *)PAYLOAD(o))
#define BOOLVAL(o)   (PAYLOAD(o)[0])
#define ELEMS(o)     ((obj_t **)PAYLOAD(o))                        /* LIST */
#define HASHES(o)    ((uint32_t *)PAYLOAD(o))                      /* ATTRS */
#define AVALUES(o, n) ((obj_t **)(PAYLOAD(o) + 4 * (uint16_t)(n)))
#define ANAMES(o, n)  ((obj_t **)(PAYLOAD(o) + 8 * (uint16_t)(n)))
#define STRBYTES(o)  ((char *)PAYLOAD(o))
#define STRCTX(o, len) (*(obj_t **)(PAYLOAD(o) + round4(len)))
#define CL_CODE(o)   (*(uint16_t *)PAYLOAD(o))                     /* THUNK/LAMBDA */
#define CL_CTAB(o)   (*(uint16_t *)(PAYLOAD(o) + 2))
#define CL_ENV(o)    (*(obj_t **)(PAYLOAD(o) + 4))
#define IND_TARGET(o) (*(obj_t **)PAYLOAD(o))                     /* INDIRECT / FORWARDED */

#define round4(n)    ((uint16_t)(((uint16_t)(n) + 3) & 0xFFFCU))

/* --- the VM's registers and tables (BYTECODE.md) --------------------------------- */
#define VSTACK_N   1024     /* 4 KB of 32-bit slots */
#define RSTACK_N   192
#define FSTACK_N   128
#define CATCH_N    12
#define CTAB_N     512      /* constant pointers, all chunks together */
#define CHUNK_MAX  128

struct chunk_info { uint16_t code; uint16_t ctab; };   /* image offset, ctab offset */

struct catch_frame {
    jmp_buf jb;
    uint16_t vsp, rsp, fstack_n;
    obj_t *env;
    const uint8_t *pc;
    obj_t **ctab;
};

extern obj_t *vstack[VSTACK_N];
extern uint16_t vsp;                /* index of the next free slot */
extern obj_t *rstack[RSTACK_N];
extern uint16_t rsp;
extern obj_t *fstack[FSTACK_N];     /* the thunks being forced, innermost last */
extern uint16_t fstack_n;
extern struct catch_frame catches[CATCH_N];
extern uint16_t catch_n;
extern obj_t *ctab_pool[CTAB_N];
extern uint16_t cnext;
extern struct chunk_info chunk_info[CHUNK_MAX];
extern uint16_t nchunks;

extern const uint8_t *pc;
extern obj_t **ctab;
extern obj_t *env;                  /* the current environment LIST, or 0 */
extern uint16_t nfree;              /* nursery bump pointer (offset in bank 1) */
extern uint16_t gc_water;           /* where the nursery starts (constants are promoted like anything else) */
extern uint16_t slot_x;             /* the card's slot * $10, or 0 */

extern obj_t *obj_true, *obj_false, *obj_null;   /* the shared singletons */
extern obj_t *str_functor;          /* "__functor" */
extern obj_t *str_success, *str_value;

#define IMAGE_PC(off)  ((const uint8_t *)(IMAGE_BASE + (uint32_t)(uint16_t)(off)))
#define PC_OFF()       ((uint16_t)(uint32_t)pc)
#define CTAB_AT(off)   ((obj_t **)((uint8_t *)ctab_pool + (uint16_t)(off)))
#define CTAB_OFF()     ((uint16_t)((uint8_t *)ctab - (uint8_t *)ctab_pool))

/* --- main.c ----------------------------------------------------------------------- */
void err_exit(uint8_t code);        /* never returns */
void nomem(void);
void type_err(void);
void badnbc(void);

/* --- data.c: allocation, stacks, values ------------------------------------------ */
obj_t *alloc(uint16_t size);        /* size a multiple of 4, >= 8 */
obj_t *new_int(int64_t v);
obj_t *str_alloc(uint16_t len);     /* a STRING with a null context, padded */
obj_t *list_alloc(uint16_t n);
obj_t *attrs_alloc(uint16_t n);
obj_t *deref_follow(obj_t *p);      /* checks, and follows INDIRECTs */
obj_t *deref_pop(void);             /* deref(pop()) */
obj_t *deref_rpop(void);            /* deref(rpop()) */
/* deref: a pointer -> the address its object can be read at. Young and static
 * objects are where they are; a mature object is copied through the bus into a
 * window (data.c) and the window's address is returned. A window is a VIEW: it
 * must never be stored -- not pushed, not put in a field -- and it is valid only
 * until the ring has turned over, so re-deref inside long loops. Store the
 * pointer you dereferenced, never its view. */
obj_t *deref(obj_t *p);             /* checks the pointer; E_BADNBC otherwise */
void   windows_invalidate(obj_t *p); /* after a write to the mature object p */
uint16_t obj_size(const obj_t *hdr);    /* from a header (gc.c) */
/* The write barrier (HEAP.md): a mature object never points into the nursery.
 * Stores into a mature object go through these, which promote the value first
 * (a collection) and write through the bus. */
void   store_field(obj_t *container, uint16_t off, obj_t *value);
void   store_byte(obj_t *container, uint16_t off, uint8_t v);
/* Forcing and popping: the ORIGINAL pointer (to store) and its view (to read). */
obj_t *force_pop_view(obj_t **view);        /* returns the original; *view = deref(original) */
/* The hot helpers are macros: ORCA/C does not inline, and a call costs more
 * than the work. Arguments are evaluated more than once -- pass lvalues.
 * (deref stays a function: as a macro at ~200 sites it cost 12 KB of code
 * for no measurable speed.) */
#define push(p)   do { if (vsp >= VSTACK_N) nomem(); vstack[vsp++] = (p); } while (0)
#define pop()     (vstack[--vsp])
#define peek()    (vstack[vsp - 1])
#define poke(p)   (vstack[vsp - 1] = (p))
#define rpush(p)  do { if (rsp >= RSTACK_N) nomem(); rstack[rsp++] = (p); } while (0)
#define rpop()    (rstack[--rsp])
#define need(o, t) do { if ((o)->tag != (t)) type_err(); } while (0)
uint32_t hash_str(obj_t *s);        /* djb2 */
int    str_eq(obj_t *a, obj_t *b);  /* 1 if equal */
int    str_cmp(obj_t *a, obj_t *b); /* 0 less, 1 equal, 2 greater */
obj_t *str_concat(obj_t *a, obj_t *b);
int    find_attr(obj_t *set, obj_t *name, uint16_t *ix);   /* set: the ORIGINAL pointer (LOOKUP if mature) */
void   attrs_put(obj_t *set, uint16_t n, uint16_t i, obj_t *name, obj_t *value);
int    values_equal(obj_t *a, obj_t *b);   /* runs bytecode; 1 if equal */
int    cmp_values(void);                   /* pops two; 0 less, 1 equal, 2 greater */
void   deep_force_top(void);

/* --- run.c: execution --------------------------------------------------------------- */
void run(void);
void force_top(void);
void force_slot(uint16_t k);        /* k = 1 is the top */
void force_two(obj_t **a, obj_t **b);            /* views */
void force_two_orig(obj_t **a, obj_t **b, obj_t **ao, obj_t **bo);   /* views and originals */
void do_call(void);                 /* f x on the stack -> the value */
void call_enter(void);
obj_t *make_apply(obj_t *f, obj_t *x);
uint16_t stack_ptr(void);
void stack_check(void);

/* --- gc.c: the nursery collector --------------------------------------------------- */
void gc_maybe(void);
void gc_collect(void);

/* --- card.c: the a2nix card -------------------------------------------------------- */
void card_find(void);
int  card_alu64(uint8_t op, const uint8_t *a, const uint8_t *b, uint8_t *r);
void card_read_at(uint32_t addr, void *dst, uint16_t n);
void card_write_at(uint32_t addr, const void *src, uint16_t n);
uint32_t card_reserve(uint16_t n);
int  card_lookup(uint32_t set, uint32_t h, uint16_t *ix, uint32_t *value);
#define A_MUL 0x02
#define A_DIV 0x03

void op_arith(uint16_t op);         /* ADD SUB MUL DIV LT, for the primops that share them */

/* --- prims.c ------------------------------------------------------------------------- */
void prims_init(void);
void op_prim(void);

#endif
