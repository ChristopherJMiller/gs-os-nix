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
#define V_MEMCMP  0x22
#define V_RESERVE 0x25
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

/* A0 <- a card address (little-endian through APORT). */
static void set_a0(uint32_t a)
{
    cw8(REG_ASEL, 0);
    cw8(REG_APORT, a & 0xFF);
    cw8(REG_APORT, (a >> 8) & 0xFF);
    cw8(REG_APORT, (a >> 16) & 0xFF);
    cw8(REG_APORT, (a >> 24) & 0xFF);
}

/* A1 <- a card address. Selecting a register rewinds the port index, so this
 * is symmetric with set_a0. NB: LOOKUP returns its value pointer in A1, so read
 * that before using A1 as a MEMCMP operand. */
static void set_a1(uint32_t a)
{
    cw8(REG_ASEL, 1);
    cw8(REG_APORT, a & 0xFF);
    cw8(REG_APORT, (a >> 8) & 0xFF);
    cw8(REG_APORT, (a >> 16) & 0xFF);
    cw8(REG_APORT, (a >> 24) & 0xFF);
}

static uint32_t get_a1(void)
{
    uint32_t a;
    cw8(REG_ASEL, 1);
    a = cr8(REG_APORT);
    a |= (uint32_t)cr8(REG_APORT) << 8;
    a |= (uint32_t)cr8(REG_APORT) << 16;
    a |= (uint32_t)cr8(REG_APORT) << 24;
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

void card_read_at(uint32_t addr, void *dst, uint16_t n)
{
    set_a0(addr);
    burst_in((uint8_t *)dst, n);
}

void card_write_at(uint32_t addr, const void *src, uint16_t n)
{
    set_a0(addr);
    burst_out((const uint8_t *)src, n);
}

/* RESERVE n bytes: the base of a headerless block (promotion, HEAP.md). */
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

/* LOOKUP: the ATTRS at card address `set`, search hash h. 1 and the index and
 * value address on a hit; 0 on NOTFOUND. The CAM compares 32 hashes a cycle;
 * the caller still confirms the name (HEAP.md). */
int card_lookup(uint32_t set, uint32_t h, uint16_t *ix, uint32_t *value)
{
    uint16_t st;
    set_a0(set);
    cw8(REG_PSEL, 0);
    cw8(REG_PPORT, h & 0xFF);
    cw8(REG_PPORT, (h >> 8) & 0xFF);
    cw8(REG_PPORT, (h >> 16) & 0xFF);
    cw8(REG_PPORT, (h >> 24) & 0xFF);
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

/* MEMCMP: n bytes at card address a against n at b. -1, 0 or 1, like strcmp;
 * 2 if the card refused (an unaligned operand), which callers read as "not
 * equal" and is why the addresses passed here are always object payloads.
 * One command replaces a byte-at-a-time compare over the bus, and it is what
 * lets find_attr confirm a LOOKUP candidate without copying the set back. */
int card_memcmp(uint32_t a, uint32_t b, uint16_t n)
{
    uint16_t st;
    set_a0(a);
    set_a1(b);
    cw8(REG_CTRL, 0x08);                        /* rewind the port indices */
    cw8(REG_LEN, n & 0xFF);
    cw8(REG_LEN, n >> 8);
    cw8(REG_LEN, 0);
    cw8(REG_LEN, 0);
    cw8(REG_CMD, V_MEMCMP);
    do st = cr8(REG_CMD); while (!(st & (ST_DONE | ST_ERR)));
    if (st & ST_ERR) {
        cw8(REG_CTRL, 0x01);
        return 2;
    }
    cw8(REG_PSEL, 0);
    return (int8_t)cr8(REG_PPORT);
}

/* ALU64: P0 <- a, P1 <- b, P2 <- op, CMD <- ALU64, poll, P3 -> r.
 * Returns 0, or 1 if the card reported an error (division by zero). */
int card_alu64(uint8_t op, const uint8_t *a, const uint8_t *b, uint8_t *r)
{
    uint16_t i, st;
    cw8(REG_PSEL, 0);
    for (i = 0; i < 8; ++i) cw8(REG_PPORT, a[i]);
    cw8(REG_PSEL, 1);
    for (i = 0; i < 8; ++i) cw8(REG_PPORT, b[i]);
    cw8(REG_PSEL, 2);
    cw8(REG_PPORT, op);
    cw8(REG_CMD, V_ALU64);
    do st = cr8(REG_CMD); while (!(st & (ST_DONE | ST_ERR)));
    if (st & ST_ERR) {
        cw8(REG_CTRL, 0x01);        /* clear the error state */
        return 1;
    }
    cw8(REG_PSEL, 3);
    for (i = 0; i < 8; ++i) r[i] = (uint8_t)cr8(REG_PPORT);
    return 0;
}
