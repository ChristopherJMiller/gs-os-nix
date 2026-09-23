/* card.c -- the VM's driver for the a2nix card (CARD-ISA.md).
 *
 * The registers are 8-bit ports at $C0nX with X = slot * $10, so every access
 * narrows the accumulator around it: a 16-bit store to a port would write the
 * next register too. The two accessors are the only assembly; the verbs are C.
 */
#pragma noroot
#pragma optimize 1

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
