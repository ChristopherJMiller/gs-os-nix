; ---------------------------------------------------------------------------
; mul64_native -- the 64 x 64 -> 64 multiply again, in 16-bit native mode.
;
; mul64_bench.s is the 8-bit emulation-mode version and was measured first. This
; is the same shift-add algorithm with the 65816's registers set to 16 bits, so
; every 64-bit operation is four word ops instead of eight byte ops. Together
; the two files measure -- rather than assume -- how much native mode buys, and
; the D5 gate for ALU64 is judged against THIS number, since the L4 VM will run
; in native mode.
;
; Same contract as the 8-bit version: the harness pokes the operands into
; MUL_A / MUL_B and reads the product from RESULT.
; ---------------------------------------------------------------------------

        .setcpu "65816"

RESULT          = $0300         ; 8-byte product, little-endian
RESULT_STAT     = $0320         ; $01 when done
MUL_A           = $0340         ; multiplicand (shifts left)
MUL_B           = $0348         ; multiplier (shifts right)

        .code
start:
        clc
        xce                     ; native mode
        rep     #$30            ; 16-bit A, X, Y
        .a16
        .i16

        stz     RESULT
        stz     RESULT+2
        stz     RESULT+4
        stz     RESULT+6

        ldy     #64

@loop:
        ; B >>= 1 across four words; the bit shifted out is the one to test.
        lsr     MUL_B+6
        ror     MUL_B+4
        ror     MUL_B+2
        ror     MUL_B
        bcc     @noadd

        ; product += A, four words, carry chaining upward
        clc
        lda     RESULT
        adc     MUL_A
        sta     RESULT
        lda     RESULT+2
        adc     MUL_A+2
        sta     RESULT+2
        lda     RESULT+4
        adc     MUL_A+4
        sta     RESULT+4
        lda     RESULT+6
        adc     MUL_A+6
        sta     RESULT+6

@noadd:
        asl     MUL_A           ; A <<= 1 across four words
        rol     MUL_A+2
        rol     MUL_A+4
        rol     MUL_A+6

        dey
        bne     @loop

        sep     #$20
        .a8
        lda     #1
        sta     RESULT_STAT
        sec
        xce                     ; back to emulation mode for the caller
        rts
