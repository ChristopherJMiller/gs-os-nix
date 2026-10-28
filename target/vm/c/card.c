/* card.c -- the VM's driver for the a2nix card (CARD-ISA.md).
 *
 * The registers are 8-bit ports at $C0nX with X = slot * $10, so every access
 * narrows the accumulator around it: a 16-bit store to a port would write the
 * next register too. The byte loops that move objects across the bus are the
 * only assembly; the verbs are C over two accessors.
 *
 * Every mature object lives on the card; the CPU reaches it through the
 * address register A0 and the DATA port, which auto-increments A0, so a
 * sequential read or write costs one bus access per byte plus four to set A0.
 */
#pragma noroot
#pragma optimize 15

#include "vm.h"

segment "core";

#define REG_CMD   0
#define REG_CTRL  1
#define REG_ASEL  2
#define REG_APORT 3
#define REG_DATA  4
#define REG_TAG   5
#define REG_PSEL  6
#define REG_PPORT 7
#define REG_LEN   8

#define CARD_ID   0xA2
#define V_HASH_INIT   0x10
#define V_HASH_STREAM 0x11
#define V_HASH_FINAL  0x12
#define V_HASH_PUSH   0x15
#define ST_READY  0x10
#define FIFO_BURST 16                           /* bytes one READY poll licenses (CARD-ISA.md) */
#define V_RESERVE 0x25
#define V_MARK    0x40
#define V_COMPACT 0x41
#define V_LOOKUP  0x30
#define V_ALU64   0x50
#define ST_DONE   0x40
#define ST_ERR    0x20

uint16_t slot_x;

/* Read the 8-bit register `off` of the card in slot_x. */
static uint16_t cr8(uint16_t off)
{
    uint16_t r = 0;
    asm {
        lda off
        clc
        adc slot_x
        tax
        sep #0x20
        lda 0xC080,x
        sta r
        rep #0x20
    }
    return r;
}

/* Write the 8-bit register `off` of the card in slot_x. */
static void cw8(uint16_t off, uint16_t v)
{
    asm {
        lda off
        clc
        adc slot_x
        tax
        sep #0x20
        lda v
        sta 0xC080,x
        rep #0x20
    }
}

/* Find the card: ID $A2 at $C0n1, slot 7 down to 1. slot_x = 0 if none. */
void card_find(void)
{
    for (slot_x = 0x70; slot_x != 0; slot_x -= 0x10)
        if (cr8(REG_CTRL) == CARD_ID) return;
}

static void card_wait(void)
{
    uint16_t st;
    do st = cr8(REG_CMD); while (!(st & (ST_DONE | ST_ERR)));
    if (st & ST_ERR) {
        cw8(REG_CTRL, 0x01);                    /* clear the error state */
        err_exit(E_NOMEM);                      /* RESERVE is the only caller that can fail */
    }
}

/* A0 <- a card address (little-endian through APORT): the select and the four
 * bytes in one block. As C it was five cw8() calls with four 32-bit shifts, and
 * ORCA/C calls ~LSHR4 for each shift (~300 cycles) and cannot inline cw8 (~76
 * cycles a call): ~1,300 cycles to write five bytes, ~15% of a promoting
 * program (a2min's function profile). Measured again after: see VM.md. */
static void set_a0(uint32_t ad)
{
    asm {
        ldx slot_x
        sep #0x20
        lda #0
        sta 0xC082,x
        lda ad
        sta 0xC083,x
        lda ad+1
        sta 0xC083,x
        lda ad+2
        sta 0xC083,x
        lda ad+3
        sta 0xC083,x
        rep #0x20
    }
}

static uint32_t get_a1(void)
{
    uint32_t a;
    uint8_t *b = (uint8_t *)&a;               /* bytes in, not shifted in (see set_a0) */
    cw8(REG_ASEL, 1);
    b[0] = (uint8_t)cr8(REG_APORT);
    b[1] = (uint8_t)cr8(REG_APORT);
    b[2] = (uint8_t)cr8(REG_APORT);
    b[3] = (uint8_t)cr8(REG_APORT);
    return a;
}

/* n bytes from RAM at src to the card from the address in A0. One bus access
 * per byte: DATA advances A0. */
static void burst_out(const uint8_t *src, uint16_t n)
{
    asm {
        ldx slot_x
        ldy #0
        sep #0x20
    lp: cpy n
        beq done
        lda [src],y
        sta 0xC084,x
        iny
        bra lp
    done: rep #0x20
    }
}

/* n bytes from the card at A0 into RAM at dst. */
static void burst_in(uint8_t *dst, uint16_t n)
{
    asm {
        ldx slot_x
        ldy #0
        sep #0x20
    lp: cpy n
        beq done
        lda 0xC084,x
        sta [dst],y
        iny
        bra lp
    done: rep #0x20
    }
}

/* n bytes to and from the parameter slot PSEL names, which auto-increments as
 * DATA does: a burst, where a cw8 or cr8 call a byte was ~76 cycles of call
 * for one register access (VM.md "Speed"). The card sees the same accesses
 * in the same order. */
static void pport_out(const uint8_t *src, uint16_t n)
{
    asm {
        ldx slot_x
        ldy #0
        sep #0x20
    lp: cpy n
        beq done
        lda [src],y
        sta 0xC087,x
        iny
        bra lp
    done: rep #0x20
    }
}

static void pport_in(uint8_t *dst, uint16_t n)
{
    asm {
        ldx slot_x
        ldy #0
        sep #0x20
    lp: cpy n
        beq done
        lda 0xC087,x
        sta [dst],y
        iny
        bra lp
    done: rep #0x20
    }
}

void card_read_at(uint32_t addr, void *dst, uint16_t n)
{
    set_a0(addr);
    burst_in((uint8_t *)dst, n);
}
/* The DATA port auto-increments A0, so a read continues where the last one
 * stopped -- valid only while nothing else has touched the card since. */
void card_read_more(void *dst, uint16_t n)
{
    burst_in((uint8_t *)dst, n);
}

void card_write_at(uint32_t addr, const void *src, uint16_t n)
{
    set_a0(addr);
    burst_out((const uint8_t *)src, n);
}

/* RESERVE n bytes: the base of a headerless block (promotion, HEAP.md). */
/* --- SHA-256 (L6: builtins.hashString, and later the .drv and store paths) --- */
/* HASH_FINAL, then 32 bytes from slot psel: P0 the digest, P3 the store-path
 * hash part storepath.v derives from it. 1, or 0 after the card raised ERR
 * (its error state cleared). */
static int hash_finish(uint8_t *out, uint8_t psel)
{
    uint16_t st;
    cw8(REG_CMD, V_HASH_FINAL);                 /* drains the FIFO, then pads */
    do st = cr8(REG_CMD); while (!(st & (ST_DONE | ST_ERR)));
    if (st & ST_ERR) { cw8(REG_CTRL, 0x01); return 0; }
    cw8(REG_PSEL, psel);
    pport_in(out, 32);
    return 1;
}

/* SHA-256 of n bytes in RAM: HASH_INIT (mandatory: it abandons any partial
 * message), HASH_PUSH, the bytes through DATA a FIFO burst per READY poll,
 * then HASH_FINAL -- the M2 demo's sequence. 0 if refused: no card, the verb
 * masked (checks.verb-economics), or the card raised ERR; the caller then
 * hashes on the CPU. */
int card_hash(const uint8_t *src, uint16_t n, uint8_t *out, uint8_t psel)
{
    uint16_t st, burst;
    if (slot_x == 0 || verb_off(VERB_NO_HASH)) return 0;
    cw8(REG_CMD, V_HASH_INIT);
    cw8(REG_CMD, V_HASH_PUSH);
    while (n) {
        do {
            st = cr8(REG_CMD);
            if (st & ST_ERR) { cw8(REG_CTRL, 0x01); return 0; }
        } while (!(st & ST_READY));
        burst = n < FIFO_BURST ? n : FIFO_BURST;
        burst_out(src, burst);
        src += burst;
        n -= burst;
    }
    return hash_finish(out, psel);
}

/* SHA-256 of n bytes already on the card at addr: HASH_STREAM, so none of
 * them cross the bus -- a mature string is hashed where it lives. */
int card_sha256_at(uint32_t addr, uint16_t n, uint8_t *out)
{
    uint16_t st;
    if (slot_x == 0 || verb_off(VERB_NO_HASH)) return 0;
    cw8(REG_CMD, V_HASH_INIT);
    set_a0(addr);
    cw8(REG_CTRL, 0x08);                        /* rewind the port indices before LEN */
    cw8(REG_LEN, n & 0xFF);
    cw8(REG_LEN, n >> 8);
    cw8(REG_LEN, 0);
    cw8(REG_LEN, 0);
    cw8(REG_CMD, V_HASH_STREAM);
    do st = cr8(REG_CMD); while (!(st & (ST_DONE | ST_ERR)));
    if (st & ST_ERR) { cw8(REG_CTRL, 0x01); return 0; }
    return hash_finish(out, 0);
}

uint32_t card_reserve(uint16_t n)
{
    cw8(REG_CTRL, 0x08);                        /* rewind the port indices (CARD-ISA.md) */
    cw8(REG_LEN, n & 0xFF);
    cw8(REG_LEN, n >> 8);
    cw8(REG_LEN, 0);
    cw8(REG_LEN, 0);
    cw8(REG_CMD, V_RESERVE);
    card_wait();
    return get_a1();
}

/* MARK: the card copies everything the n 32-bit roots at card address `roots`
 * reach into its to-space and writes each root's new address over it; null,
 * young and static pointers pass through (CARD-ISA.md). O(survivors), and no
 * object crosses the bus. Then COMPACT flips the halves. */
void card_mark(uint32_t roots, uint16_t n)
{
    cw8(REG_CTRL, 0x08);                        /* rewind the port indices (CARD-ISA.md) */
    set_a0(roots);
    cw8(REG_LEN, n & 0xFF);
    cw8(REG_LEN, n >> 8);
    cw8(REG_LEN, 0);
    cw8(REG_LEN, 0);
    cw8(REG_CMD, V_MARK);
    card_wait();
}

void card_compact(void)
{
    cw8(REG_CMD, V_COMPACT);
    card_wait();
}

/* LOOKUP: the ATTRS at card address `set`, search hash h. 1 and the index and
 * value address on a hit; 0 on NOTFOUND. The CAM compares 32 hashes a cycle;
 * the caller still confirms the name (HEAP.md). */
int card_lookup(uint32_t set, uint32_t h, uint16_t *ix, uint32_t *value)
{
    uint16_t st;
    set_a0(set);
    cw8(REG_PSEL, 0);
    pport_out((const uint8_t *)&h, 4);
    cw8(REG_CMD, V_LOOKUP);
    do st = cr8(REG_CMD); while (!(st & (ST_DONE | ST_ERR)));
    if (st & ST_ERR) {
        cw8(REG_CTRL, 0x01);
        return 0;
    }
    *value = get_a1();
    cw8(REG_PSEL, 1);
    *ix = cr8(REG_PPORT);
    *ix |= cr8(REG_PPORT) << 8;
    return 1;
}

/* ALU64: P0 <- a, P1 <- b, P2 <- op, CMD <- ALU64, poll, P3 -> r.
 * Returns 0, or 1 if the card reported an error (division by zero). */
int card_alu64(uint8_t op, const uint8_t *a, const uint8_t *b, uint8_t *r)
{
    uint16_t st;
    cw8(REG_PSEL, 0);
    pport_out(a, 8);
    cw8(REG_PSEL, 1);
    pport_out(b, 8);
    cw8(REG_PSEL, 2);
    cw8(REG_PPORT, op);
    cw8(REG_CMD, V_ALU64);
    do st = cr8(REG_CMD); while (!(st & (ST_DONE | ST_ERR)));
    if (st & ST_ERR) {
        cw8(REG_CTRL, 0x01);        /* clear the error state */
        return 1;
    }
    cw8(REG_PSEL, 3);
    pport_in(r, 8);
    return 0;
}
