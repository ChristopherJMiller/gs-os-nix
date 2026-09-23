; ---------------------------------------------------------------------------
; card.s -- the VM's driver for the a2nix card (CARD-ISA.md).
;
; NOT INCLUDED BY vm.s: written for slice 4 step C (promotion), paused when the
; on-target code moved to C (DECISIONS.md D18). Reference for the C driver.
;
; Everything here is 8-bit register I/O at $C0nX with X = slot_x, so the
; accumulator is narrowed around each block: a 16-bit store to a port would
; write the next register too. The address registers and LEN are little-endian
; integers streamed a byte at a time; DATA auto-increments A0.
;
; No card (slot_x = 0) is E_NOCARD at the first promotion: the VM cannot run
; the D6 design without the mature space.
; ---------------------------------------------------------------------------

V_RESERVE       = $25
V_LOOKUP        = $30
E_NOTFOUND      = $02           ; the card's error code, in STATUS bits 3..0

; card_a0_p0: A0 <- the 32-bit pointer in p0.
card_a0_p0:
        ldx     slot_x
        sep     #$20
        .a8
        lda     #0
        sta     REG_ASEL,x
        lda     p0
        sta     REG_APORT,x
        lda     p0+1
        sta     REG_APORT,x
        lda     p0+2
        sta     REG_APORT,x
        lda     p0+3
        sta     REG_APORT,x
        rep     #$20
        .a16
        rts

; card_len_a: LEN <- A (16-bit, zero-extended); rewinds the port index first.
card_len_a:
        sta     t3
        ldx     slot_x
        sep     #$20
        .a8
        lda     #$08            ; CTRL b3: RESET_PORTS
        sta     REG_CTRL,x
        lda     t3
        sta     REG_LEN,x
        lda     t3+1
        sta     REG_LEN,x
        lda     #0
        sta     REG_LEN,x
        sta     REG_LEN,x
        rep     #$20
        .a16
        rts

; card_wait: poll until DONE or ERR; A (8-bit) = the status byte. X = slot.
card_wait:
        .a8
@w:     lda     REG_CMD,x
        bit     #(ST_DONE|ST_ERR)
        beq     @w
        rts
        .a16

; card_reserve: A = byte count -> xbase (4 bytes) = the reserved block's
; address, from A1. E_NOMEM if the card refuses (heap full).
card_reserve:
        jsr     card_len_a
        ldx     slot_x
        sep     #$20
        .a8
        lda     #V_RESERVE
        sta     REG_CMD,x
        jsr     card_wait
        and     #ST_ERR
        bne     @err
        lda     #1
        sta     REG_ASEL,x
        lda     REG_APORT,x
        sta     xbase
        lda     REG_APORT,x
        sta     xbase+1
        lda     REG_APORT,x
        sta     xbase+2
        lda     REG_APORT,x
        sta     xbase+3
        rep     #$20
        .a16
        rts
        .a8
@err:   rep     #$20
        .a16
        jmp     nomem

; card_read: n0 bytes from the card at pointer p0 into bank 0 at t6.
; Clobbers t3, X, Y, A.
card_read:
        jsr     card_a0_p0
        ldx     slot_x
        ldy     #0
        sep     #$20
        .a8
@r:     cpy     n0
        beq     @done
        lda     REG_DATA,x
        sta     (t6),y
        iny
        bra     @r
@done:  rep     #$20
        .a16
        rts

; card_write: n0 bytes from bank 0 at t6 to the card at pointer p0.
card_write:
        jsr     card_a0_p0
        ldx     slot_x
        ldy     #0
        sep     #$20
        .a8
@w:     cpy     n0
        beq     @done
        lda     (t6),y
        sta     REG_DATA,x
        iny
        bra     @w
@done:  rep     #$20
        .a16
        rts

; card_lookup: the ATTRS at card address p0, search hash h -> C set and
; ix = the index, p1 = the value pointer (from A1); C clear on NOTFOUND.
; The CAM compares 32 hashes a cycle; the caller still confirms the name.
card_lookup:
        jsr     card_a0_p0
        ldx     slot_x
        sep     #$20
        .a8
        lda     #0
        sta     REG_PSEL,x
        lda     h
        sta     REG_PPORT,x
        lda     h+1
        sta     REG_PPORT,x
        lda     h+2
        sta     REG_PPORT,x
        lda     h+3
        sta     REG_PPORT,x
        lda     #V_LOOKUP
        sta     REG_CMD,x
        jsr     card_wait
        and     #ST_ERR
        bne     @miss
        lda     #1
        sta     REG_ASEL,x
        lda     REG_APORT,x
        sta     p1
        lda     REG_APORT,x
        sta     p1+1
        lda     REG_APORT,x
        sta     p1+2
        lda     REG_APORT,x
        sta     p1+3
        lda     #1
        sta     REG_PSEL,x
        lda     REG_PPORT,x
        sta     ix
        lda     REG_PPORT,x
        sta     ix+1
        rep     #$20
        .a16
        sec
        rts
        .a8
@miss:  lda     #$01            ; CTRL b0: clear the error state
        sta     REG_CTRL,x
        rep     #$20
        .a16
        clc
        rts
