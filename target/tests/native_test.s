; ---------------------------------------------------------------------------
; native_test -- exercises every 65816 native-mode feature a2min claims to model.
;
; Each numbered block writes one 16-bit word into the table at NT_RESULT. The
; EXPECTED values are NOT in this file: they live in sim/a2min.cpp's selftest
; table, worked out by hand from the WDC data sheet. That split is deliberate --
; if this program and the interpreter were both written from the same mistaken
; belief, a self-checking test would pass anyway. Keeping the oracle on the other
; side of the fence means a wrong belief has to be wrong twice, independently.
;
; Run with:  just selftest
;
; Scratch memory map (bank 0 unless noted):
;   $0400  result table            $0500  (sr,S),Y data
;   $0600  MVN source              $0900  MVP destination
;   $0A00  RMW word                $0B00  16-bit index targets
;   $0C00  dp,X / abs,Y targets    $0D00  direct page for the TCD test
;   bank 1: $0500 [dp] data, $0600 JSL routine, $0700 PLB data, $0800 MVN
;           destination, $0A00 JML routine.   bank 2: $0000 long test.
; ---------------------------------------------------------------------------

        .setcpu "65816"

NT_RESULT       = $0400
RESULT_STAT     = $0320

        .zeropage
ptr:            .res 3          ; 24-bit pointer for [dp] modes
ptr2:           .res 2          ; 16-bit pointer for (dp) modes
word:           .res 2

        .code
start:
        clc
        xce                     ; native mode
        rep     #$30            ; 16-bit A, X, Y
        .a16
        .i16

        ; 0: 16-bit immediate load
        lda     #$BEEF
        sta     NT_RESULT+0

        ; 1, 2: 16-bit add wraps at bit 16 and carries out
        clc
        lda     #$FFFF
        adc     #1
        sta     NT_RESULT+2
        lda     #0
        adc     #0              ; picks up the carry from above
        sta     NT_RESULT+4

        ; 3: SEP to 8 bits keeps B, the high byte; XBA swaps them
        lda     #$1234
        sep     #$20
        .a8
        lda     #$56            ; C = $1256
        xba                     ; C = $5612
        rep     #$20
        .a16
        sta     NT_RESULT+6

        ; 4, 5: direct page relocation
        lda     #$0D00
        tcd
        lda     #$A5A5
        sta     $10             ; D + $10 = $0D10
        lda     #0
        tdc                     ; A = D
        sta     NT_RESULT+10
        lda     $0D10           ; absolute, no D involved
        sta     NT_RESULT+8
        lda     #0
        tcd                     ; D back to 0 so the zp symbols below are right

        ; 6: [dp] -- a 24-bit pointer into bank 1
        lda     #$0BAD
        sta     f:$010500
        lda     #$0500
        sta     ptr
        lda     #$0001
        sta     ptr+2           ; bank byte = $01 (the extra byte lands in ptr2, harmless)
        lda     [ptr]
        sta     NT_RESULT+12

        ; 7: [dp],Y
        lda     #$C0DE
        sta     f:$010510
        ldy     #$10
        lda     [ptr],y
        sta     NT_RESULT+14

        ; 8: long,X store that crosses into bank 2, then a long load
        lda     #$F00D
        ldx     #4
        sta     f:$01FFFC,x     ; = $020000
        lda     #0
        lda     f:$020000
        sta     NT_RESULT+16

        ; 9: JSL into bank 1. The routine is built in place: lda #$0042 ; rtl
        lda     #$42A9          ; A9 42
        sta     f:$010600
        lda     #$6B00          ; 00 6B
        sta     f:$010602
        lda     #0
        jsl     $010600
        sta     NT_RESULT+18

        ; 10: PLB -- absolute addressing goes through the data bank register
        lda     #$D00D
        sta     f:$010700
        phb
        pea     $0001
        plb                     ; DBR = $01 (pops the low byte)
        lda     $0700           ; = $010700
        plb                     ; the leftover $00
        plb                     ; the saved DBR
        sta     NT_RESULT+20

        ; 11: PEA then stack-relative
        pea     $1234
        lda     1,s
        sta     NT_RESULT+22
        pla

        ; 12: (sr,S),Y
        lda     #$5A5A
        sta     $0504
        pea     $0500
        ldy     #4
        lda     (1,s),y
        sta     NT_RESULT+24
        pla                     ; (a first draft popped before storing -- PLA loads A)

        ; 13: 16-bit X
        ldx     #$ABCD
        stx     NT_RESULT+26

        ; 14: MVN 16 bytes from bank 0 $0600 to bank 1 $0800. Source is 0..15.
        sep     #$20
        .a8
        ldx     #0
@fill:  txa
        sta     $0600,x
        inx
        cpx     #16
        bne     @fill
        rep     #$20
        .a16
        ldx     #$0600
        ldy     #$0800
        lda     #15             ; count - 1
        mvn     #$00, #$01      ; source bank, destination bank
        phk
        plb                     ; MVN left DBR = $01; restore it
        lda     f:$01080E
        sta     NT_RESULT+28

        ; 15: MVP copies downward
        ldx     #$060F
        ldy     #$090F
        lda     #15
        mvp     #$00, #$00
        lda     $0900
        sta     NT_RESULT+30

        ; 16: 16-bit read-modify-write chain
        lda     #1
        sta     $0A00
        inc     $0A00           ; 2
        asl     $0A00           ; 4
        clc
        rol     $0A00           ; 8
        dec     $0A00           ; 7
        lsr     $0A00           ; 3, carry = 1
        ror     $0A00           ; $8001 if the carry enters bit 15
        lda     $0A00
        sta     NT_RESULT+32

        ; 17: 16-bit compare
        lda     #$0100
        cmp     #$00FF          ; carry set only if the whole word is compared
        lda     #0
        adc     #0
        sta     NT_RESULT+34

        ; 18: PHD/PLD
        lda     #$0500
        tcd
        phd
        lda     #0
        tcd
        pld                     ; D = $0500 again
        tdc
        sta     NT_RESULT+36
        lda     #0
        tcd

        ; 19: TCS/TSC, restoring the real stack pointer afterwards
        tsc
        tax
        lda     #$01F0
        tcs
        tsc
        sta     NT_RESULT+38
        txa
        tcs

        ; 20: PEI
        lda     #$7777
        sta     word
        pei     (word)
        pla
        sta     NT_RESULT+40

        ; 21: PER -- pushes the address of a label; subtract it back out
        per     @here
@here:  pla
        sec
        sbc     #.loword(@here)
        inc     a
        sta     NT_RESULT+42

        ; 22: BRL
        lda     #$600D
        brl     @ok
        lda     #$BAD0
@ok:    sta     NT_RESULT+44

        ; 23: JML into bank 1 and back. Routine: lda #$1111 ; jml @back
        lda     #$11A9
        sta     f:$010A00
        lda     #$5C11
        sta     f:$010A02
        lda     #.loword(@back)
        sta     f:$010A04
        lda     #$0000          ; bank byte of @back, plus one spare byte
        sta     f:$010A06
        jml     $010A00
@back:  sta     NT_RESULT+46

        ; 24: TXY / TYX
        ldx     #$1357
        txy
        ldx     #0
        tyx
        stx     NT_RESULT+48

        ; 25, 26, 27: back to emulation mode and out again
        lda     #$1234
        ldx     #$5678
        sec
        xce                     ; emulation: A and X are 8 bits, B keeps $12
        .a8
        .i8
        sta     NT_RESULT+50    ; one byte; NT_RESULT+51 stays 0
        stx     NT_RESULT+52
        clc
        xce
        rep     #$30
        .a16
        .i16
        sta     NT_RESULT+54    ; C = $1234 if B survived
        ; 37: X was narrowed on the way INTO emulation mode, so now that it is 16
        ; bits again the high byte must be gone. (Case 37's slot, written here
        ; because a later ldx would clobber the evidence.)
        stx     NT_RESULT+74

        ; 28: abs,X with a 16-bit index
        lda     #$2468
        ldx     #$0300
        sta     $0800,x         ; = $0B00
        lda     $0B00
        sta     NT_RESULT+56

        ; 29: (dp),Y with a 16-bit Y
        lda     #$9999
        sta     $0B02
        lda     #$0B00
        sta     ptr2
        ldy     #2
        lda     (ptr2),y
        sta     NT_RESULT+58

        ; 30: (dp,X) -- pointer at ptr2 = ptr + 3, reached via X = 3 from ptr
        ldx     #3
        lda     (ptr,x)
        sta     NT_RESULT+60

        ; 31: dp,X with a 16-bit X (D + $00 + $0C00)
        lda     #$4242
        sta     $0C00
        ldx     #$0C00
        lda     $00,x
        sta     NT_RESULT+62

        ; 32: abs,Y with a 16-bit Y
        lda     #$4343
        sta     $0C02
        ldy     #$0C02
        lda     a:$0000,y
        sta     NT_RESULT+64

        ; 33, 34: 16-bit subtract with borrow
        sec
        lda     #0
        sbc     #1
        sta     NT_RESULT+66
        lda     #0
        adc     #0              ; carry is clear after a borrow
        sta     NT_RESULT+68

        ; 35: 16-bit PHA/PLA
        lda     #$CAFE
        pha
        lda     #0
        pla
        sta     NT_RESULT+70

        ; 36: 16-bit PHX / PLY
        ldx     #$F00F
        phx
        ldy     #0
        ply
        sty     NT_RESULT+72

        ; 38: a 16-bit add that crosses bit 8 must NOT set the carry
        clc
        lda     #$00FF
        adc     #1
        lda     #0
        adc     #0
        sta     NT_RESULT+76

        ; done: report, and leave the CPU as we found it
        sep     #$20
        .a8
        lda     #1
        sta     RESULT_STAT
        sec
        xce
        rts
