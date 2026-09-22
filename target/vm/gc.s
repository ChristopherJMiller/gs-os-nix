; ---------------------------------------------------------------------------
; gc.s -- the nursery collector (HEAP.md, "The VM and the two spaces").
;
; Runs only between two bytecode instructions (see `run`), when the live
; pointers are exactly the roots listed below and no zero-page scratch holds
; an object. A Cheney copy of the live graph goes into a STAGING area in
; another bank at the SAME low addresses the survivors will have afterwards,
; so pointers written during the copy are already the final ones:
;
;   step B (this file, today): the compacted survivors are block-moved back
;           into the nursery -- an in-RAM semispace collection; nothing has
;           crossed the bus, the VM's data stays young;
;   step C: the staging area is RESERVEd and streamed to the card instead, and
;           the roots are rewritten to mature addresses (promotion, D6).
;
; Constants materialised from the .nbc sit below gc_water and are pinned: never
; moved, never scanned (they hold no pointers). The VM's own static objects
; live in its binary ($2000-$4FFF) and are left alone by address.
;
; Roots: the value stack; env; RSTACK; FSTACK; the env of every catch frame.
;
; Not a pointer: word 0 of a THUNK/LAMBDA/BLACKHOLE (a code address and a
; constant-table address). The card's tracer would chase it -- HEAP.md records
; that as an open item for card-side collection.
; ---------------------------------------------------------------------------

STAGE_BANK      = $01           ; the staging bank (any bank > 0 with RAM)
GC_MARGIN       = $0800         ; collect when less than this is free
GC_STRESS       = $0BF0         ; harness flag: nonzero = collect at every boundary

; --- the trigger ------------------------------------------------------------------
; Called from `run` before every dispatch. Collects when the nursery is nearly
; full, or (stress mode) whenever anything has been allocated since the last
; collection.
gc_maybe:
        lda     GC_STRESS
        and     #$00FF
        bne     @stress
        lda     nfree
        cmp     #NURSERY_END-GC_MARGIN
        bcs     gc_collect
        rts
@stress:
        lda     nfree
        cmp     gc_water
        bne     gc_collect
        rts

; --- the collection -------------------------------------------------------------------
gc_collect:
        lda     gc_water
        sta     gfree           ; to-space bump pointer (low word; bank STAGE_BANK)
        sta     gscan
        ; roots: the value stack
        lda     #VSTACK
        sta     t6
@vs:    lda     t6
        cmp     vsp
        beq     @vs_done
        sta     t0
        lda     (t0)
        sta     p0
        ldy     #2
        lda     (t0),y
        sta     p0+2
        jsr     evac
        lda     t6
        sta     t0
        lda     p0
        sta     (t0)
        ldy     #2
        lda     p0+2
        sta     (t0),y
        lda     t6
        clc
        adc     #4
        sta     t6
        bra     @vs
@vs_done:
        ; env
        lda     env
        sta     p0
        lda     env+2
        sta     p0+2
        jsr     evac
        lda     p0
        sta     env
        lda     p0+2
        sta     env+2
        ; RSTACK
        ldx     #0
@rs:    cpx     rsp
        beq     @rs_done
        jsr     evac_at_x_rstack
        inx
        inx
        inx
        inx
        bra     @rs
@rs_done:
        ; FSTACK
        ldx     #0
@fs:    cpx     fstack_n
        beq     @fs_done
        jsr     evac_at_x_fstack
        inx
        inx
        inx
        inx
        bra     @fs
@fs_done:
        ; catch frames
        ldx     #0
@cf:    txa
        lsr
        lsr
        lsr
        lsr
        cmp     catch_n
        beq     @cf_done
        lda     CATCH+4,x
        sta     p0
        lda     CATCH+6,x
        sta     p0+2
        phx
        jsr     evac
        plx
        lda     p0
        sta     CATCH+4,x
        lda     p0+2
        sta     CATCH+6,x
        txa
        clc
        adc     #16
        tax
        bra     @cf
@cf_done:
        ; the Cheney scan of to-space
@scan:  lda     gscan
        cmp     gfree
        beq     @scanned
        jsr     scan_one
        bra     @scan
@scanned:
        ; step B: block-move the survivors back into the nursery
        lda     gfree
        sec
        sbc     gc_water
        beq     @empty
        dec     a
        ldx     gc_water
        ldy     gc_water
        mvn     #STAGE_BANK, #$00
        phk
        plb                     ; the VM's absolute addressing assumes bank 0
@empty: lda     gfree
        sta     nfree
        rts

evac_at_x_rstack:
        lda     RSTACK,x
        sta     p0
        lda     RSTACK+2,x
        sta     p0+2
        phx
        jsr     evac
        plx
        lda     p0
        sta     RSTACK,x
        lda     p0+2
        sta     RSTACK+2,x
        rts

evac_at_x_fstack:
        lda     FSTACK,x
        sta     p0
        lda     FSTACK+2,x
        sta     p0+2
        phx
        jsr     evac
        plx
        lda     p0
        sta     FSTACK,x
        lda     p0+2
        sta     FSTACK+2,x
        rts

; --- evacuation ---------------------------------------------------------------------
; evac: p0 = a pointer -> p0 = its pointer after the collection. Null, mature,
; static (in the VM binary) and pinned-constant pointers pass through. A
; young object is copied to to-space once; its old header becomes FORWARDED
; with the new pointer, so a second reference finds the copy.
; Clobbers t0, t1, t2, t3, lp, X, Y.
evac:
        lda     p0+2
        bpl     @done           ; mature (or null)
        lda     p0
        cmp     #NURSERY
        bcc     @done           ; static: in the VM's binary
        cmp     gc_water
        bcc     @done           ; a pinned constant
        cmp     #NURSERY_END
        bcs     @done           ; not an object at all (never happens; be safe)
        sta     t0
        lda     (t0)
        and     #$00FF
        cmp     #T_FORWARDED
        bne     @copy
        ldy     #4
        lda     (t0),y
        sta     p0
        iny
        iny
        lda     (t0),y
        sta     p0+2
@done:  rts
@copy:  jsr     obj_size        ; t2 = size of the object at t0
        lda     gfree
        sta     t3              ; its new address (low word)
        ldx     t0
        tay
        lda     t2
        dec     a
        mvn     #$00, #STAGE_BANK
        phk
        plb
        lda     t3
        clc
        adc     t2
        sta     gfree
        ; forward the old object
        sep     #$20
        .a8
        lda     #T_FORWARDED
        sta     (t0)
        rep     #$20
        .a16
        ldy     #4
        lda     t3
        sta     (t0),y
        sta     p0
        iny
        iny
        lda     #$8000
        sta     (t0),y
        sta     p0+2
        rts

; obj_size: the size in bytes of the young object at t0 -> t2. Marks in the
; aux high bits are ignored. Clobbers t2, t3, Y.
obj_size:
        lda     (t0)
        and     #$00FF
        sta     t3              ; tag
        ldy     #1
        lda     (t0),y          ; aux, low 16 bits
        ldx     t3
        cpx     #T_INT
        beq     @s12
        cpx     #T_STRING
        beq     @str
        cpx     #T_LIST
        beq     @list
        cpx     #T_ATTRS
        beq     @attrs
        cpx     #T_THUNK
        beq     @s12
        cpx     #T_LAMBDA
        beq     @s12
        cpx     #T_BLACKHOLE
        beq     @s12
        lda     #8              ; BOOL, NULL, INDIRECT
        sta     t2
        rts
@s12:   lda     #12
        sta     t2
        rts
@str:   jsr     round4
        clc
        adc     #8
        sta     t2
        rts
@list:  asl
        asl
        clc
        adc     #4
        sta     t2
        rts
@attrs: asl
        asl
        sta     t2
        asl
        clc
        adc     t2
        clc
        adc     #4
        sta     t2
        rts

; --- the scan --------------------------------------------------------------------------
; scan_one: the to-space object at gscan (bank STAGE_BANK): evacuate every
; pointer field, then advance gscan by its size. Fields are read and written
; with long addressing through lp.
scan_one:
        lda     gscan
        sta     lp
        sep     #$20
        .a8
        lda     #STAGE_BANK
        sta     lp+2
        rep     #$20
        .a16
        lda     [lp]
        and     #$00FF
        sta     t4              ; tag
        ldy     #1
        lda     [lp],y
        and     #$3FFF          ; aux low bits (the marks are above)
        sta     t5              ; count / length
        ldx     t4
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
        beq     @skip12
        lda     #8
        bra     @advance
@skip12:
        lda     #12
        bra     @advance
@closure:
        ldy     #8              ; the env pointer; word 0 is code, not a pointer
        jsr     evac_field
        lda     #12
        bra     @advance
@indirect:
        ldy     #4
        jsr     evac_field
        lda     #8
        bra     @advance
@string:
        lda     t5
        jsr     round4
        clc
        adc     #4
        tay                     ; the context pointer
        jsr     evac_field
        lda     t5
        jsr     round4
        clc
        adc     #8
        bra     @advance
@list:  ldy     #4
        ldx     t5
@le:    cpx     #0
        beq     @list_done
        jsr     evac_field
        iny
        iny
        iny
        iny
        dex
        bra     @le
@list_done:
        lda     t5
        asl
        asl
        clc
        adc     #4
        bra     @advance
@attrs: lda     t5              ; skip the hash column: 4 + 4n
        asl
        asl
        clc
        adc     #4
        tay
        lda     t5
        asl
        tax                     ; 2n pointer words: values then names
@ae:    cpx     #0
        beq     @attrs_done
        jsr     evac_field
        iny
        iny
        iny
        iny
        dex
        bra     @ae
@attrs_done:
        lda     t5
        asl
        asl
        sta     t2
        asl
        clc
        adc     t2
        clc
        adc     #4
@advance:
        clc
        adc     gscan
        sta     gscan
        rts

; evac_field: the 32-bit field at [lp],Y is evacuated in place. Preserves X, Y.
evac_field:
        phx
        phy
        lda     [lp],y
        sta     p0
        iny
        iny
        lda     [lp],y
        sta     p0+2
        lda     lp              ; evac clobbers lp's low word? it does not, but
        pha                     ; keep the scan pointer safe regardless
        jsr     evac
        pla
        sta     lp
        sep     #$20
        .a8
        lda     #STAGE_BANK
        sta     lp+2
        rep     #$20
        .a16
        ply
        phy
        lda     p0
        sta     [lp],y
        iny
        iny
        lda     p0+2
        sta     [lp],y
        ply
        plx
        rts
