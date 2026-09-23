; ---------------------------------------------------------------------------
; promote.s -- slice 4 step C: promotion of the staging area to the card.
;
; NOT INCLUDED BY vm.s. This is the assembly draft of promotion (RESERVE + one
; DATA burst, roots rewritten by translate), paused when the on-target code
; moved to C (DECISIONS.md D18). It references STATICS/STATICS_END (the VM's
; static pointer table, not yet defined), win_invalidate (the copy-window
; cache) and card.s. Kept as the reference for the C port of gc step C.
; ---------------------------------------------------------------------------

evac_at_x_statics:
        lda     STATICS,x
        sta     p0
        lda     STATICS+2,x
        sta     p0+2
        phx
        jsr     evac
        plx
        lda     p0
        sta     STATICS,x
        lda     p0+2
        sta     STATICS+2,x
        rts

; --- promotion --------------------------------------------------------------------
; translate: p0 (a young pointer) -> the card address of its copy.
translate:
        lda     p0+2
        bpl     @done
        lda     p0
        sec
        sbc     #NURSERY
        clc
        adc     xbase
        sta     p0
        lda     xbase+2
        adc     #0
        sta     p0+2
@done:  rts

; promote_stream: emit the staging area [NURSERY, gfree) of bank STAGE_BANK
; to the card at xbase, one object at a time, translating pointer fields.
promote_stream:
        lda     xbase
        sta     p0
        lda     xbase+2
        sta     p0+2
        jsr     card_a0_p0      ; A0 <- xbase; DATA auto-increments from here
        lda     #NURSERY
        sta     gscan
@obj:   lda     gscan
        cmp     gfree
        jeq     @done
        lda     gscan
        sta     lp
        sep     #$20
        .a8
        lda     #STAGE_BANK
        sta     lp+2
        rep     #$20
        .a16
        jsr     ptr_range       ; t4 = first pointer word, t5 = last (excl.), t2 = size
        ; header: 4 raw bytes
        ldy     #0
        jsr     emit_raw_word
        jsr     emit_raw_word
        stz     kx              ; word index
@w:     lda     kx
        asl
        asl
        clc
        adc     #4
        cmp     t2
        bcs     @next
        tay
        lda     kx
        cmp     t4
        bcc     @raw
        cmp     t5
        bcs     @raw
        ; a pointer word
        lda     [lp],y
        sta     p0
        iny
        iny
        lda     [lp],y
        sta     p0+2
        jsr     translate
        jsr     emit_p0
        inc     kx
        bra     @w
@raw:   jsr     emit_raw_word
        jsr     emit_raw_word
        inc     kx
        bra     @w
@next:  lda     gscan
        clc
        adc     t2
        sta     gscan
        bra     @obj
@done:  rts

; emit_raw_word: the 2 bytes at [lp],Y to DATA; Y += 2.
emit_raw_word:
        lda     [lp],y
        ldx     slot_x
        sep     #$20
        .a8
        sta     REG_DATA,x
        xba
        sta     REG_DATA,x
        rep     #$20
        .a16
        iny
        iny
        rts

; emit_p0: the 4 bytes of p0 to DATA.
emit_p0:
        ldx     slot_x
        sep     #$20
        .a8
        lda     p0
        sta     REG_DATA,x
        lda     p0+1
        sta     REG_DATA,x
        lda     p0+2
        sta     REG_DATA,x
        lda     p0+3
        sta     REG_DATA,x
        rep     #$20
        .a16
        rts

; ptr_range: for the object at [lp]: t2 = its size, and the payload words
; [t4, t5) are pointers (HEAP.md's trace table; word 0 of a closure is code).
ptr_range:
        lda     [lp]
        and     #$00FF
        sta     t3
        ldy     #1
        lda     [lp],y
        and     #$3FFF
        sta     n1              ; count / length
        stz     t4
        stz     t5
        ldx     t3
        cpx     #T_LIST
        beq     @list
        cpx     #T_ATTRS
        beq     @attrs
        cpx     #T_THUNK
        beq     @closure
        cpx     #T_LAMBDA
        beq     @closure
        cpx     #T_BLACKHOLE
        beq     @closure
        cpx     #T_INDIRECT
        beq     @indirect
        cpx     #T_STRING
        beq     @string
        cpx     #T_INT
        beq     @s12
        lda     #8
        sta     t2
        rts
@s12:   lda     #12
        sta     t2
        rts
@closure:
        lda     #1
        sta     t4
        lda     #2
        sta     t5
        lda     #12
        sta     t2
        rts
@indirect:
        lda     #1
        sta     t5
        lda     #8
        sta     t2
        rts
@string:
        lda     n1
        jsr     round4
        lsr
        lsr
        sta     t4
        inc     a
        sta     t5
        lda     n1
        jsr     round4
        clc
        adc     #8
        sta     t2
        rts
@list:  lda     n1
        sta     t5
        asl
        asl
        clc
        adc     #4
        sta     t2
        rts
@attrs: lda     n1
        sta     t4
        asl
        adc     n1              ; 3n (carry is clear: 2n + n)
        sta     t5
        lda     n1
        asl
        asl
        sta     t2
        asl
        clc
        adc     t2
        clc
        adc     #4
        sta     t2
        rts

; promote_roots: every root that still holds a young pointer now names the
; copy on the card.
promote_roots:
        lda     #VSTACK
        sta     t6
@vs:    lda     t6
        cmp     vsp
        beq     @vs_done
        jsr     translate_at_t6
        lda     t6
        clc
        adc     #4
        sta     t6
        bra     @vs
@vs_done:
        lda     env
        sta     p0
        lda     env+2
        sta     p0+2
        jsr     translate
        lda     p0
        sta     env
        lda     p0+2
        sta     env+2
        lda     #RSTACK
        sta     t6
@rs:    lda     t6
        sec
        sbc     #RSTACK
        cmp     rsp
        beq     @rs_done
        jsr     translate_at_t6
        lda     t6
        clc
        adc     #4
        sta     t6
        bra     @rs
@rs_done:
        lda     #FSTACK
        sta     t6
@fs:    lda     t6
        sec
        sbc     #FSTACK
        cmp     fstack_n
        beq     @fs_done
        jsr     translate_at_t6
        lda     t6
        clc
        adc     #4
        sta     t6
        bra     @fs
@fs_done:
        lda     #STATICS
        sta     t6
@st:    lda     t6
        cmp     #STATICS_END
        beq     @st_done
        jsr     translate_at_t6
        lda     t6
        clc
        adc     #4
        sta     t6
        bra     @st
@st_done:
        lda     #CTAB
        sta     t6
@ct:    lda     t6
        cmp     cnext
        beq     @ct_done
        jsr     translate_at_t6
        lda     t6
        clc
        adc     #4
        sta     t6
        bra     @ct
@ct_done:
        lda     #CATCH+4
        sta     t6
        stz     t5
@cf:    lda     t5
        cmp     catch_n
        beq     @cf_done
        jsr     translate_at_t6
        lda     t6
        clc
        adc     #16
        sta     t6
        inc     t5
        bra     @cf
@cf_done:
        rts

translate_at_t6:
        lda     t6
        sta     t0
        lda     (t0)
        sta     p0
        ldy     #2
        lda     (t0),y
        sta     p0+2
        jsr     translate
        lda     t6
        sta     t0
        lda     p0
        sta     (t0)
        ldy     #2
        lda     p0+2
        sta     (t0),y
        rts

