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
 * Allocation itself never collects. Collections run at run()'s dispatch, in
 * nursery_reserve(), and in store_field() or a thunk update with the remembered
 * set full (gc.c); so those may collect too. And any collection may be followed
 * by one of mature space, which moves mature objects as well (HEAP.md
 * "Collecting mature space"): the rule is the same for both kinds of pointer.
 */
#ifndef VM_H
#define VM_H

/* ORCA/C's own lint, every check, as errors: an undefined (unprototyped)
 * function, an unused variable or a constant that a conversion changes stops
 * the build. Every .c includes this first, so it covers all the VM's code. */
#pragma lint -1

#include <stdint.h>
#include <string.h>
#include <setjmp.h>
#include "opcodes.h"

/* --- the memory map (docs/VM.md) ------------------------------------------------ */
#define RESULT_ADDR      0x0300UL   /* 4-byte pointer to the result object */
#define DIAG_ADDR        0x0310UL   /* the VM's state on error, see diag_write() */
#define RESULT_STAT_ADDR 0x0320UL   /* $01 done, $FF error */
#define RESULT_ERR_ADDR  0x0321UL
/* Phase counters, written beside DIAG at exit and printed by a2min.
 *
 * They exist because three separate performance questions this VM raised were
 * answered wrongly by reasoning about the code and only settled by measurement
 * -- and the one number the harness already had (bus accesses) is what settled
 * each of them. Everything else was a single undifferentiated cycle count, so
 * "where did 60M cycles go" took an hour of hypotheses. These are the cheap
 * half of the answer: how MANY times each expensive thing happened.
 *
 * Every counter sits on a cold path -- a collection, a promotion, a window
 * copy, a thunk force -- so none of them is on the dispatch loop. */
#define STATS_ADDR       0x0340UL   /* 9 x u32, live -- see the note above */
#define STATS_N          9
/* Counted IN PLACE, at a fixed address, rather than in globals copied out at
 * exit. The first version did the latter and was useless for the one run that
 * mattered: a program stopped by the cycle budget never reaches the copy, so
 * the counters printed as zero exactly when they were needed. A volatile
 * absolute increment costs the same as a global one here (both are bank 0), so
 * there is nothing to trade away. */
#define STAT(i)          (*(volatile uint32_t *)(STATS_ADDR + 4UL * (i)))
#define ST_COLLECT       STAT(0)    /* collections */
#define ST_BARRIER       STAT(1)    /* of those, forced by a FULL remembered set */
#define ST_PROMOTED      STAT(2)    /* bytes streamed to the card */
#define ST_VIEWS         STAT(3)    /* mature reads that copied (cache misses) */
#define ST_VIEW_BYTES    STAT(4)    /* bytes those copies moved */
#define ST_TURNOVER      STAT(5)    /* window-ring turnovers */
#define ST_FORCES        STAT(6)    /* thunks forced */
#define ST_REMEMBERED    STAT(7)    /* young pointers stored into mature fields */
#define ST_MATURE        STAT(8)    /* collections of mature space (MARK and COMPACT) */

/* The harness flags. The first two were at $0BF0, which was inside the C stack's
 * range then ($0400..$1FFF): a deep enough recursion would have overwritten them,
 * silently turning GC stress off or a masked verb back on -- the second of
 * which would quietly corrupt a measurement. $0330 is below the stack floor. */
#define GC_STRESS_ADDR   0x0330UL   /* a2min sets it: collect at every boundary */
#define VERB_MASK_ADDR   0x0331UL   /* a2min sets it: verbs to refuse and do on the CPU
                                       instead, so checks.verb-economics can price each
                                       one on a real program (docs/CARD-ISA.md) */
#define RSET_LIMIT_ADDR  0x0332UL   /* a2min sets it: shrink the remembered set to this
                                       many entries (0 = RSET_N), so the tests can make
                                       it fill -- the one path a normal run never takes */
#define RESERVE_STRESS_ADDR 0x0333UL /* a2min sets it: every nursery_reserve() collects,
                                       needed or not -- so each call site's claim that
                                       nothing live is in a C local is actually tested */
#define GSOS_ADDR        0x0334UL   /* a2min sets it: GS/OS answers the file calls (GSOS.md);
                                       0, and the builtins that need it are E_GSOS by name */
#define MATURE_STRESS_ADDR 0x0335UL /* a2min sets it: k, and every k-th promotion is followed
                                       by a collection of mature space, needed or not -- with
                                       GC stress, every fixture tests the moving collector */
#define VERB_NO_LOOKUP   0x01U
#define VERB_NO_ALU64    0x02U
#define VERB_NO_HASH     0x04U   /* SHA-256: HASH_PUSH / HASH_STREAM + HASH_FINAL */
#define verb_off(bit)    (*(volatile uint8_t *)VERB_MASK_ADDR & (bit))
/* The C stack is bank 0's top, above the program (VM.md "The C stack", D31):
 * omf2bin's stub starts S at STACK_TOP, and nothing under STACK_FLOOR is the
 * stack's -- under it is bank 0's room to grow, then its globals. bank-gate
 * holds bank 0's end under STACK_FLOOR, the stub to STACK_TOP, and a2min's
 * tripwire to both. */
#define STACK_TOP        0xBFFFU
#define STACK_FLOOR      0x7000U
#define STACK_MIN        0x7400U    /* the C stack (S) must stay above this at every check,
                                       which leaves 1 KB above STACK_FLOOR: the deepest
                                       stack measured between two checks is 682 bytes
                                       (a fixed-output derivation; VM.md "The C stack").
                                       Deeper paths ask for their own room: stack_need */
_Static_assert(STACK_MIN - STACK_FLOOR == 0x0400U, "1 KB over the deepest path between two checks");
#define FLOAT_STACK      1600U      /* float.c's parsing and formatting, under one call:
                                       f_strtod's frame alone is 1,068 bytes, g_digits' 1,038 */
#define TOML_STACK       2600U      /* tm_parse: 32 nested arrays or tables, then f_strtod */

#define YOUNG_TAG        0x80000000UL
#define NURSERY_BANK     0x01
#define NURSERY_BASE     0x00010000UL
#define NURSERY_END      0xFFF0U    /* offsets below this are the nursery */
#define STAGE_BASE       0x00020000UL
#define IMAGE_BASE       0x00030000UL
#define IMAGE_MAX        0xFFF0UL   /* the image must fit below APPLY_CODE */
#define APPLY_CODE_OFF   0xFFF0U    /* main.c copies apply_code[] there at start */
#define WINDOW_BASE      0x00050000UL   /* bank 5: copies of mature objects (data.c) */
#define WINDOW_RING      0xF000U    /* the ring of copies of mature objects */
#define WINDOW_MAX       0x5000U    /* the largest object that can be viewed at all */
#define WINDOW_FLOOR     0x4000U    /* turn the ring over at a bytecode boundary past this,
                                       so every instruction starts with WINDOW_RING -
                                       WINDOW_FLOOR = 44 KB free: more than the 2 x
                                       WINDOW_MAX + change any one of them can hold live
                                       at once, so the ring NEVER wraps mid-instruction
                                       and no live view is ever overwritten (data.c) */

/* The remembered set (gc.c): the 4 KB of bank 5 above the window ring. An
 * entry is 10 bytes, so 256 of them take 2.5 KB. */
#define RSET_BASE        0x0005F000UL
#define RSET_N           256

_Static_assert(WINDOW_BASE + WINDOW_RING == RSET_BASE, "the remembered set is just above the window ring");
_Static_assert(RSET_BASE + 10UL * RSET_N <= 0x00060000UL, "the remembered set fits in bank 5");
_Static_assert(WINDOW_RING - WINDOW_FLOOR >= 2UL * WINDOW_MAX, "two views of the largest objects fit an instruction");
_Static_assert(IMAGE_MAX <= APPLY_CODE_OFF, "the .nbc image ends below the apply code");
#ifdef __ORCAC__
_Static_assert(sizeof(int) == 2 && sizeof(long) == 4 && sizeof(void *) == 4, "ORCA/C's data model");
#endif

#define GC_MARGIN        0x0800U    /* collect when less than this is free */
#define NURSERY_LOW      0x2000U    /* with no card, a collection that leaves less than
                                       this free is E_NOMEM, not a storm (gc.c) */

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
#define T_FLOAT      0x0D   /* 8 bytes, IEEE-754 binary64 bits (docs/FLOAT.md) */

/* aux2 marks: bit 7 = visited (deep force), bit 6 = a `with` environment */
#define AUX_MARK     0x80
#define AUX_WITH     0x40

typedef struct obj {
    uint8_t tag;
    uint8_t aux0, aux1, aux2;   /* 24-bit little-endian count/length, marks above */
} obj_t;
_Static_assert(sizeof(obj_t) == 4, "a header is four bytes (HEAP.md)");

#define PBITS(p)     ((uint32_t)(p))
/* Bytes and words of a 32-bit value, read out of its storage. ORCA/C compiles a
 * 32-bit shift as a call to ~LSHR4/~SHL4, which shifts a bit at a time: `a >> 16`
 * cost ~300 cycles, and those calls were 28% of eval-okay-foldlStrict's cycles
 * (a2min's function profile, A2MIN_SYMBOLS). A load costs a few. The argument
 * must be an lvalue -- an rvalue fails to compile rather than misbehaving. */
#define BYTE_OF(x, i) (((const uint8_t *)&(x))[i])
#define HI16(x)       (((const uint16_t *)&(x))[1])
#define LO16(x)       (((const uint16_t *)&(x))[0])
/* A null pointer, x a variable: its halves ORed -- ORCA/C's == 0 on a pointer
 * was ~13 instructions (VM.md "Speed") */
#define IS_NULL(x)    ((LO16(x) | HI16(x)) == 0)
#define IS_YOUNG(p)  (HI16(p) == 0x8001U)   /* in the nursery */
#define IS_STATIC(p) (HI16(p) == 0x8000U)   /* in this program */
#define IS_RAM(p)    ((HI16(p) & 0x8000U) != 0)                  /* dereferenceable; p a variable:
                                                                   the 32-bit & was ~20 instructions */
#define IS_MATURE(p) ((p) != 0 && !IS_RAM(p))                     /* on the card */
#define IS_WINDOW(p) (HI16(p) == 0x8005U)   /* a copy of a mature object */
#define LOW16(p)     ((uint16_t)PBITS(p))
#define YOUNG_PTR(off) ((obj_t *)(YOUNG_TAG | NURSERY_BASE | (uint32_t)(uint16_t)(off)))
#define STATIC_PTR(o)  ((obj_t *)(YOUNG_TAG | (uint32_t)(o)))

#define AUX16(o)     ((uint16_t)((o)->aux0 | ((uint16_t)(o)->aux1 << 8)))
/* The header as two words -- tag and aux0, then aux1 and aux2 = 0 -- not four
 * byte stores, each with its own SEP/REP (VM.md "Speed") */
#define SET_HDR(o, t, n) (((uint16_t *)(o))[0] = (uint16_t)((t) | ((uint16_t)(n) << 8)), \
                          ((uint16_t *)(o))[1] = (uint16_t)((uint16_t)(n) >> 8))
#define PAYLOAD(o)   ((uint8_t *)(o) + 4)
#define INTVAL(o)    (*(int64_t *)PAYLOAD(o))
#define BOOLVAL(o)   (PAYLOAD(o)[0])
#define ELEMS(o)     ((obj_t **)PAYLOAD(o))                        /* LIST */
#define HASHES(o)    ((uint32_t *)PAYLOAD(o))                      /* ATTRS */
#define AVALUES(o, n) ((obj_t **)(PAYLOAD(o) + 4 * (uint16_t)(n)))
#define ANAMES(o, n)  ((obj_t **)(PAYLOAD(o) + 8 * (uint16_t)(n)))
#define APOS(o, n)    ((uint16_t *)(PAYLOAD(o) + 12 * (uint16_t)(n)))   /* positions, 0 none (HEAP.md) */
/* Bank 7 (VM.md): a primop's scratch while it runs no bytecode -- regex.c's
 * and toml.c's, and listToAttrs' positions -- which no collection touches. */
#define SCRATCH_BASE  0x00070000UL
#define SCRATCH_W(i)  (((uint16_t *)SCRATCH_BASE)[(uint16_t)(i)])
#define ATTR_RESERVE  16    /* what a reservation counts an attribute: 12 bytes, a position, padding */
#define STRBYTES(o)  ((char *)PAYLOAD(o))
#define STRCTX(o, len) (*(obj_t **)(PAYLOAD(o) + round4(len)))
#define CL_CODE(o)   (*(uint16_t *)PAYLOAD(o))                     /* THUNK/LAMBDA */
#define CL_CTAB(o)   (*(uint16_t *)(PAYLOAD(o) + 2))
#define CL_ENV(o)    (*(obj_t **)(PAYLOAD(o) + 4))
#define IND_TARGET(o) (*(obj_t **)PAYLOAD(o))                     /* INDIRECT / FORWARDED */

#define round4(n)    ((uint16_t)(((uint16_t)(n) + 3) & 0xFFFCU))

/* --- the VM's registers and tables (BYTECODE.md) --------------------------------- */
#define VSTACK_N   1024     /* 4 KB of 32-bit slots */
/* One root and, for a thunk, one force a level of nesting: 320 and 256 are
 * above what the C stack holds of the shapes that cost it least per level --
 * a call ~72 bytes, 270 levels; a thunk forced inside a thunk ~130, 151 -- so
 * it is the C stack that ends those (VM.md "The C stack"). A deepSeq of nested
 * lists spends ~21 bytes a level and is ended here, at ~316. */
#define RSTACK_N   320
#define FSTACK_N   256
#define CATCH_N    12
#define CTAB_N     512      /* constant pointers, all chunks together */
/* `builtins` used as a value materialises a lambda per primop, and the 47
 * primops need 73 chunks between them (one per argument, curried), so a program
 * that says `with builtins;` spends more than half of 128 before its own code.
 * A chunk costs 4 bytes here and 4 in imports[]; 256 is 1 KB of bank 0. The
 * loader refuses an image with more (main.c), loudly. */
#define CHUNK_MAX  256

/* Mature space (HEAP.md "Collecting mature space"). The card's heap is two
 * halves of CARD_HALF bytes (rtl/a2nix_card.v SPACE_SIZE: A from $10, B from
 * CARD_HALF + $10), and a promotion only ever goes into the current one. After
 * every promotion, if less than MATURE_MARGIN is left in it -- the most the
 * next promotion can need, a whole nursery, and the largest root list -- the
 * card's MARK and COMPACT collect it; if that still leaves less, live data has
 * filled the half, and it is E_NOMEM rather than a collection at every
 * promotion. */
#define CARD_HALF     0x00080000UL
#define ROOTS_MAX     (VSTACK_N + 1 + RSTACK_N + FSTACK_N + CATCH_N + CTAB_N + CHUNK_MAX + 1)
#define MATURE_MARGIN (0x00010000UL + 4UL * ROOTS_MAX)

struct chunk_info { uint16_t code; uint16_t ctab; };   /* image offset, first constant's index in ctab_pool */

struct catch_frame {
    jmp_buf jb;
    uint16_t vsp, rsp, fstack_n;
    obj_t *env;
    const uint8_t *pc;
    uint16_t ctab;
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
/* One thunk per imported FILE, not per import site: Nix evaluates an imported
 * file once and shares the value, and that is observable -- `import ./f ==
 * import ./f` is true because both sides are the SAME object. CLOSED_THUNK
 * memoises here, so the sharing does not depend on where the import appears.
 * A root, like the constant tables. */
extern obj_t *imports[CHUNK_MAX];


extern uint16_t nchunks;

extern const uint8_t *pc;
extern uint16_t ctab;              /* the running chunk's first constant, an index into ctab_pool:
                                       CONST k is ctab_pool[ctab + k], an absolute indexed load
                                       where a pointer was 32-bit arithmetic (VM.md "Speed") */
extern obj_t *env;                  /* the current environment LIST, or 0 */
extern uint16_t nfree;              /* nursery bump pointer (offset in bank 1) */
extern uint16_t gc_water;           /* where the nursery starts (constants are promoted like anything else) */
extern uint16_t slot_x;             /* the card's slot * $10, or 0 */

extern obj_t *obj_true, *obj_false, *obj_null;   /* the shared singletons */
extern obj_t *str_functor;          /* "__functor" */
extern obj_t *str_success, *str_value;

#define IMAGE_PC(off)  ((const uint8_t *)(IMAGE_BASE + (uint32_t)(uint16_t)(off)))
#define PC_OFF()       ((uint16_t)(uint32_t)pc)

/* --- main.c ----------------------------------------------------------------------- */
_Noreturn void err_exit(uint8_t code);
_Noreturn void nomem(void);
_Noreturn void type_err(void);
_Noreturn void badnbc(void);

/* --- data.c: allocation, stacks, values ------------------------------------------ */
obj_t *alloc(uint16_t size);        /* size a multiple of 4, >= 8 */
/* alloc, inline: the nursery's bump, for the hot allocations (VM.md "Speed") */
#define ALLOC(o, n) do { if ((uint16_t)(n) > (uint16_t)(NURSERY_END - nfree)) nomem(); \
                         ((uint16_t *)&(o))[0] = nfree; ((uint16_t *)&(o))[1] = 0x8001U; \
                         nfree += (n); } while (0)   /* YOUNG_PTR(nfree), by halves; o a variable */
obj_t *new_int(int64_t v);
obj_t *str_alloc(uint16_t len);     /* a STRING with a null context, padded */
obj_t *path_alloc(uint16_t len);    /* a PATH, no context word (HEAP.md) */
obj_t *path_join(const char *a, uint16_t na, const char *b, uint16_t nb);   /* canonPath(a ++ b); caller reserves na+nb+1 */
obj_t *list_alloc(uint16_t n);
obj_t *attrs_alloc(uint16_t n);
void   attr_name(void);                     /* ATTR_NAME (prims.c) */
void   attrs_dynamic(void);                 /* ATTRS_DYN (prims.c): reads its operands at pc */
void   attrs_literal(void);                 /* ATTRS (prims.c): likewise */
void   op_update(void);                     /* UPDATE (prims.c) */
void   lambda_args(void);                   /* LAMBDA_ARGS (prims.c): likewise */
obj_t *deref_follow(obj_t *p);      /* checks, and follows INDIRECTs */
obj_t *deref_pop(void);             /* deref(pop()) */
obj_t *deref_rpop(void);            /* deref(rpop()) */
/* deref: a pointer -> the address its object can be read at. Young and static
 * objects are where they are; a mature object is copied through the bus into a
 * window (data.c) and the window's address is returned. A window is a VIEW: it
 * must never be stored -- not pushed, not put in a field -- and it is valid
 * until the next BYTECODE BOUNDARY, which is where the ring turns over. So any
 * loop that calls back into Nix must re-deref (prims.c RELEM), and anything
 * that outlives the instruction is the pointer, never the view. */
obj_t *deref(obj_t *p);             /* checks the pointer; E_BADNBC otherwise */
/* deref's young case inline, for the hot paths (VM.md "Speed"): a pointer
 * with the RAM bit is its own view, anything else -- mature, or null, which
 * deref refuses -- goes to deref. A call costs ~200 cycles for those three
 * lines. p must be an lvalue (HI16 takes its address), which also keeps a
 * side effect out of the twice-read argument. */
#define DEREF(p)  ((HI16(p) & 0x8000U) ? (p) : deref(p))
/* A tag force_top would act on: THUNK, INDIRECT, BLACKHOLE (not LAMBDA, 9).
 * t is read twice: a plain variable. */
#define NEEDS_FORCE(t)  ((uint8_t)((t) - T_THUNK) < 4 && (t) != T_LAMBDA)
/* Force slot p (an lvalue) through force_top, or skip the call when it is
 * already a value -- most of the time (VM.md "Speed") */
#define FORCE_TOP()     do { uint8_t ft_ = DEREF(peek())->tag; if (NEEDS_FORCE(ft_)) force_top(); } while (0)
#define FORCE_SLOT2()   do { uint8_t ft_ = DEREF(vstack[vsp - 2])->tag; if (NEEDS_FORCE(ft_)) force_slot(2); } while (0)
obj_t *list_elem(obj_t *l, uint16_t i, uint16_t count);   /* element i; reads one element of a long mature list */
void   windows_invalidate(obj_t *p); /* after a write to the mature object p */
/* The ring turns over only at a bytecode boundary, where no view is live. The
 * dispatch loop does the test itself; windows_turnover() is the rare case. */
extern uint16_t win_ring;
void   windows_turnover(void);
#define windows_maybe() do { if (win_ring > WINDOW_FLOOR) windows_turnover(); } while (0)
uint16_t obj_size(const obj_t *hdr);    /* from a header (gc.c) */
/* The write barrier (HEAP.md): a mature object never points into the nursery
 * EXCEPT through a field in the remembered set. Stores into a mature object go
 * through these: they write through the bus, and a young pointer stored that
 * way is recorded (gc.c) for the next collection to promote and rewrite. Only
 * when the set is full does a store cost a collection. */
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
/* set is the ORIGINAL pointer, never a view. On a hit the VALUE comes back too,
 * so no caller has to deref the set to read it (data.c). */
int    find_attr(obj_t *set, obj_t *name, uint16_t *ix, obj_t **val);
int    has_attr(obj_t *set, obj_t *name);   /* when only the answer is wanted */
void   attrs_put(obj_t *set, uint16_t n, uint16_t i, obj_t *name, obj_t *value);
int    values_equal(obj_t *a, obj_t *b);   /* runs bytecode; 1 if equal */
int    attrs_equal(obj_t *a, obj_t *b);    /* prims.c: values_equal for two sets, derivations by outPath */
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
void stack_need(uint16_t n);        /* E_NOMEM unless n bytes more than STACK_MIN are free */

/* --- gc.c: the nursery collector --------------------------------------------------- */
void gc_collect(void);
/* Whether a boundary collects (run()'s loop head): normally for nursery
 * pressure. Under GC stress the byte at GC_STRESS_ADDR is a period k: every
 * k-th boundary. k = 1 promotes every value before it can ever be stored
 * young, so it never exercises the remembered set; k = 2 or 3 leaves values
 * young across some stores (D20). */
extern uint8_t gc_tick;
extern uint8_t gc_period;   /* GC_STRESS_ADDR's byte, read once at entry: a2min sets it
                               before the run, and the loop read it through a long
                               pointer at every boundary (VM.md "Speed") */
/* Room for one operation's allocation: collect now if fewer than `bytes` are
 * free (E_NOMEM if a collection cannot make them). Collections otherwise run
 * only at bytecode boundaries with a GC_MARGIN of 2 KB, so an operation that
 * allocates in proportion to its input -- genList 300 is 13 KB -- used to be
 * E_NOMEM or not depending on where the nursery happened to be. Call it ONLY
 * where every live object is on a stack: before the operation pops its
 * operands into C locals, or once everything it holds is on RSTACK. Returns
 * 1 if it collected: every view taken before it must then be taken again. */
int nursery_reserve(uint16_t bytes);
int nursery_reserve_n(uint16_t n, uint16_t per, uint16_t fixed);   /* fixed + n * per, E_NOMEM past a bank */
void view_two(obj_t **a, obj_t **b);   /* force the top two in place and view them; both stay on the stack */
extern uint16_t rs_n;               /* entries in the remembered set */
#define rs_full() (rs_n >= (*(volatile uint8_t *)RSET_LIMIT_ADDR ? \
                            *(volatile uint8_t *)RSET_LIMIT_ADDR : RSET_N))
void remember(obj_t *container, uint16_t off);   /* the field now holds a young pointer */

/* --- card.c: the a2nix card -------------------------------------------------------- */
void card_find(void);
int  card_alu64(uint8_t op, const uint8_t *a, const uint8_t *b, uint8_t *r);
void card_read_at(uint32_t addr, void *dst, uint16_t n);
void card_read_more(void *dst, uint16_t n);      /* continue from where the last read stopped */
void card_write_at(uint32_t addr, const void *src, uint16_t n);
uint32_t card_reserve(uint16_t n);
void card_mark(uint32_t roots, uint16_t n);   /* MARK: evacuate n roots at `roots`, each rewritten */
void card_compact(void);                       /* COMPACT: the flip */
int  card_lookup(uint32_t set, uint32_t h, uint16_t *ix, uint32_t *value);
/* HASH_PUSH n bytes in RAM, then HASH_FINAL; the 32 bytes of slot psel: P0 the
 * digest, P3 the store-path hash part (the digest folded and in nixbase32). 0 if
 * refused. */
int  card_hash(const uint8_t *src, uint16_t n, uint8_t *out, uint8_t psel);
#define card_sha256(s, n, o)    card_hash((s), (n), (o), 0)
#define card_storepath(s, n, o) card_hash((s), (n), (o), 3)
int  card_sha256_at(uint32_t addr, uint16_t n, uint8_t *out);     /* bytes already on the card */
void sha256_cpu(const uint8_t *src, uint16_t n, uint8_t *out);    /* sha256.c: no card, or masked */
void nix32_fold(const uint8_t *dig, char *out);   /* sha256.c: what P3 holds, on the CPU */
#define A_MUL 0x02
#define A_DIV 0x03

void op_arith(uint16_t op);         /* ADD SUB MUL DIV LT, for the primops that share them */
/* float.c: IEEE-754 binary64 in software, a float being its bit pattern
 * (docs/FLOAT.md); bit for bit the host's, rounding to nearest, ties to even. */
#define F_FORMAT_MAX 330                    /* %f of the largest double, and a sign */
uint64_t f_add(uint64_t a, uint64_t b);
uint64_t f_sub(uint64_t a, uint64_t b);
uint64_t f_mul(uint64_t a, uint64_t b);
uint64_t f_div(uint64_t a, uint64_t b);     /* b != 0 */
uint64_t f_from_int(int64_t v);
int      f_eq(uint64_t a, uint64_t b);      /* -0 == +0; a NaN is unequal to all */
int      f_lt(uint64_t a, uint64_t b);      /* false if either is a NaN */
int      f_round_int(uint64_t a, int up, int64_t *out);   /* floor/ceil; 0 if out of int64 */
uint16_t f_format(uint64_t a, char *buf);   /* printf("%f"), exactly: its length */
#define F_JSON_MAX 32                       /* f_json's longest: -1.7976931348623157e+308 */
uint16_t f_json(uint64_t a, char *buf);     /* toJSON's text, nlohmann's: its length */
uint64_t f_strtod(const char *s, uint16_t n);   /* a JSON number token, as strtod reads it */
uint16_t f_g(uint64_t a, char *buf);        /* printf("%g"), exactly (toXML): its length, at most 13 */
/* ...and the VM's floats, on views: */
obj_t   *new_float(uint64_t bits);          /* 12 bytes, as new_int: inside the boundary's margin */
void     float_arith(uint16_t op, obj_t *a, obj_t *b);   /* ADD SUB MUL DIV NEG, one a FLOAT: pushes it */
int      num_cmp(obj_t *a, obj_t *b);       /* one a FLOAT: 0 <, 1 ==, 2 >, 3 unordered; -1 not numbers */
int      num_order(obj_t *a, obj_t *b);     /* num_cmp, but E_TYPE for -1 (cmp_values) */
int      num_pair(obj_t *a, obj_t *b);      /* E_TYPE unless both numbers; 1 if either is a FLOAT */
obj_t   *float_str(obj_t *v);               /* toString's %f, a fresh STRING; may collect */
void     prim_floor(void);
void     prim_ceil(void);

/* prims.c: Nix 2.34's checked integer arithmetic -- E_OVERFLOW, never a wrap. */
int64_t add_checked(int64_t x, int64_t y);
int64_t sub_checked(int64_t x, int64_t y);
void alu_overflow(uint8_t op, const uint8_t *a, const uint8_t *b);   /* after a MUL or DIV */

/* --- prims.c ------------------------------------------------------------------------- */
void prims_init(void);
void op_prim(void);
/* Nix's coerceToString on the top of the stack (runs bytecode): a set through
 * __toString or outPath; with to_string, builtins.toString's wider rules. */
#define CO_MORE   1     /* toString's wider rules: ints, bools, null, lists */
#define CO_PATH   2     /* a path becomes its text; without it E_UNSUP (a store copy, L7) */
#define CO_PLAIN  0                     /* `+` and "${}" */
#define CO_STRING (CO_MORE | CO_PATH)    /* builtins.toString */
#define CO_DRV    CO_MORE               /* derivation attributes (DERIVATION.md) */
void coerce_top(uint8_t how);
/* INTERPOLATE n, and a string `+` (n = 2): the n parts on the stack coerced as
 * "${}" does, concatenated, their contexts united (DERIVATION.md "String
 * context"). Runs bytecode. */
void interpolate(uint16_t n);
/* Every derivation the program has made, for its dependents' hashes: a chain
 * of [drvPath, modulo hash in hex, next] LISTs, or 0. A root (gc.c). */
extern obj_t *drv_table;

#endif
