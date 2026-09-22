; ---------------------------------------------------------------------------
; mul64_bench -- a 64 x 64 -> 64 multiply in 65816 software, for the D5 gate.
;
; ALU64 on the card only earns its 1462 LUT4 if it beats what the CPU can do
; alone. This is the CPU's best honest attempt: the classic shift-add loop, 64
; iterations, each a conditional 64-bit add plus a 64-bit shift of both operands.
;
; Emulation mode, 8-bit accumulator, so every 64-bit operation is eight byte ops.
; mul64_native.s is the same algorithm with 16-bit registers. This file once
; predicted native mode would "roughly halve" the cycles; the measurement says
; 1.7x (9675 -> 5693), because every 16-bit memory operand costs a cycle more.
; Both numbers are in CARD-ISA.md; the D5 gate is judged against the native one.
;
; The harness pokes the operands into MUL_A / MUL_B before entry and reads the
; product from RESULT afterwards, so nothing here is hardcoded and the expected
; value comes from the host's own int64 multiply.
; ---------------------------------------------------------------------------

        .setcpu "65816"

RESULT          = $0300         ; 8-byte product, little-endian
RESULT_STAT     = $0320         ; $01 when done
MUL_A           = $0340         ; 8 bytes, little-endian: multiplicand (shifts left)
MUL_B           = $0348         ; 8 bytes, little-endian: multiplier (shifts right)

        .zeropage
count:          .res 1

        .code
start:
        sec
        xce                     ; emulation mode
        cld

        lda     #0              ; product = 0
        ldx     #7
@clr:   sta     RESULT,x
        dex
        bpl     @clr

        lda     #64
        sta     count

@loop:
        lda     MUL_B           ; if the multiplier's low bit is set, add
        and     #1
        beq     @noadd

        ; product += A, 64 bits, byte by byte, carry chaining from low to high.
        ;
        ; The loop counter must not touch the carry flag, and CPX does: an earlier
        ; version used INX/CPX #8 and every ADC after the first consumed CPX's carry
        ; instead of the previous add's. The product came out with the right low
        ; byte and a wrong byte 1 -- a2min emulated the mistake faithfully. So X runs
        ; from $F8 up through $FF and INX alone ends the loop when it wraps to zero.
        ;
        ; abs,X adds X as an UNSIGNED byte, so the base must sit $F8 BELOW the target:
        ; base+$F8 = RESULT. (A first attempt used RESULT+8, treating X as -8, and
        ; quietly accumulated the product at $0400.)
        clc
        ldx     #$F8
@add:   lda     RESULT-$F8,x
        adc     MUL_A-$F8,x
        sta     RESULT-$F8,x
        inx
        bne     @add

@noadd:
        asl     MUL_A           ; A <<= 1 across 8 bytes
        rol     MUL_A+1
        rol     MUL_A+2
        rol     MUL_A+3
        rol     MUL_A+4
        rol     MUL_A+5
        rol     MUL_A+6
        rol     MUL_A+7

        lsr     MUL_B+7         ; B >>= 1 across 8 bytes
        ror     MUL_B+6
        ror     MUL_B+5
        ror     MUL_B+4
        ror     MUL_B+3
        ror     MUL_B+2
        ror     MUL_B+1
        ror     MUL_B

        dec     count
        bne     @loop

        lda     #1
        sta     RESULT_STAT
        rts
