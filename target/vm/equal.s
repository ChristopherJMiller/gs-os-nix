; ---------------------------------------------------------------------------
; equal.s -- structural equality and deep force. Included by vm.s.
;
; Both recurse through the CPU stack and keep their loop state there too,
; addressed stack-relative (`lda 1,s`), because the recursive call may
; clobber every zero-page scratch variable (see data.s).
; ---------------------------------------------------------------------------

; values_equal: p0 == p1 -> Z set if equal, as Nix's `==`: forces both;
; INT/BOOL/NULL by value, STRING by bytes, LIST and ATTRS element-wise
; (recursively), functions never equal unless the same object.
values_equal:
        jsr     rpush_p0        ; a, a root while b is forced
        lda     p1
        sta     p0
        lda     p1+2
        sta     p0+2
        jsr     push_p0
        jsr     force_top
        jsr     pop_p0
        jsr     rpush_p0        ; b (forced), a root while a is forced
        lda     rsp
        sec
        sbc     #8
        tax
        jsr     rget_x          ; a, as the collector may have moved it
        jsr     push_p0
        jsr     force_top
        jsr     pop_p0
        lda     p0
        pha
        lda     p0+2
        pha                     ; a, forced (no bytecode from here on)
        jsr     rpop_p0
        lda     p0
        sta     p1
        lda     p0+2
        sta     p1+2            ; b
        jsr     rpop_p0         ; drop a's entry
        pla
        sta     p0+2
        pla
        sta     p0
        jsr     deref
        jsr     deref_p1
        lda     t0
        cmp     t1
        jeq     @eq
        lda     (t0)
        and     #$00FF
        sta     t2
        lda     (t1)
        and     #$00FF
        cmp     t2
        jne     @ne
        cmp     #T_INT
        beq     @int
        cmp     #T_BOOL
        beq     @bool
        cmp     #T_NULL
        jeq     @eq
        cmp     #T_STRING
        beq     @str
        cmp     #T_LIST
        jeq     @list
        cmp     #T_ATTRS
        jeq     @attrs
        jmp     @ne
@int:   ldy     #4
        ldx     #4
@iw:    lda     (t0),y
        cmp     (t1),y
        jne     @ne
        iny
        iny
        dex
        bne     @iw
        jmp     @eq
@bool:  ldy     #4
        lda     (t0),y
        and     #$00FF
        sta     t2
        lda     (t1),y
        and     #$00FF
        cmp     t2
        jne     @ne
        jmp     @eq
@str:   jmp     str_eq

        ; frame for the two loops: a 7,s  b 5,s  n 3,s  i 1,s
        ; The two loops keep a and b on the root stack (rbase+0, rbase+4) and
        ; n, i, rbase on the CPU stack: n 5,s  i 3,s  rbase 1,s.
@list:  jsr     aux_t0
        sta     n0
        jsr     aux_t1
        cmp     n0
        jne     @ne
        jsr     rpush_p0        ; a (p0/p1 still hold the forced pointers)
        lda     p1
        sta     p0
        lda     p1+2
        sta     p0+2
        jsr     rpush_p0        ; b
        lda     n0
        pha
        lda     #0
        pha
        lda     rsp
        sec
        sbc     #8
        pha
@ll:    lda     3,s
        cmp     5,s
        jeq     @loop_eq
        lda     1,s
        tax
        jsr     rget_x
        jsr     deref
        lda     3,s
        jsr     elem_t0         ; a[i]
        lda     p0
        sta     p2
        lda     p0+2
        sta     p2+2
        lda     1,s
        clc
        adc     #4
        tax
        jsr     rget_x
        jsr     deref
        lda     3,s
        jsr     elem_t0         ; b[i]
        lda     p0
        sta     p1
        lda     p0+2
        sta     p1+2
        lda     p2
        sta     p0
        lda     p2+2
        sta     p0+2
        jsr     values_equal
        jne     @loop_ne
        lda     3,s
        inc     a
        sta     3,s
        jmp     @ll

@attrs: jsr     aux_t0
        sta     n0
        jsr     aux_t1
        cmp     n0
        jne     @ne
        jsr     rpush_p0        ; a
        lda     p1
        sta     p0
        lda     p1+2
        sta     p0+2
        jsr     rpush_p0        ; b
        lda     n0
        pha
        lda     #0
        pha
        lda     rsp
        sec
        sbc     #8
        pha
@al:    lda     3,s
        cmp     5,s
        jeq     @loop_eq
        lda     5,s
        sta     n0
        lda     1,s
        tax
        jsr     rget_x
        jsr     deref
        lda     3,s
        jsr     attr_name       ; a's name i
        lda     p0
        sta     p2
        lda     1,s
        clc
        adc     #4
        tax
        jsr     rget_x
        jsr     deref
        lda     3,s
        jsr     attr_name       ; b's name i
        jsr     deref_p0_t1     ; t1 = b's name
        lda     p2
        sta     p0
        jsr     deref           ; t0 = a's name
        jsr     str_eq
        jne     @loop_ne
        lda     5,s
        sta     n0
        lda     1,s
        tax
        jsr     rget_x
        jsr     deref
        lda     3,s
        jsr     attr_value      ; a's value i
        lda     p0
        sta     p2
        lda     p0+2
        sta     p2+2
        lda     1,s
        clc
        adc     #4
        tax
        jsr     rget_x
        jsr     deref
        lda     3,s
        jsr     attr_value      ; b's value i
        lda     p0
        sta     p1
        lda     p0+2
        sta     p1+2
        lda     p2
        sta     p0
        lda     p2+2
        sta     p0+2
        jsr     values_equal
        jne     @loop_ne
        lda     3,s
        inc     a
        sta     3,s
        jmp     @al

@loop_eq:
        pla
        sta     rsp
        pla
        pla
        jmp     @eq
@loop_ne:
        pla
        sta     rsp
        pla
        pla
@ne:    lda     #1
        rts
@eq:    lda     #0
        rts

; deep_force_top: force the top of the stack and, recursively, every element
; of a LIST and every value of an ATTRS. The stack top is unchanged (its
; object has been updated in place).
deep_force_top:
        jsr     force_top
        jsr     peek_p0
        jsr     deref
        lda     (t0)
        and     #$00FF
        cmp     #T_LIST
        jeq     @list
        cmp     #T_ATTRS
        jeq     @attrs
        rts
        ; A container being visited carries a mark in bit 15 of its header's
        ; second word (bit 7 of aux2, never used by a real count), so a cyclic
        ; structure -- `let as = { y = as; }` -- terminates, as Nix's deepSeq
        ; does. The mark is cleared on the way out. The container's pointer
        ; lives on the root stack; n 5,s  i 3,s  rbase 1,s.
@list:  ldy     #2
        lda     (t0),y
        jmi     @cycle
        ora     #$8000
        sta     (t0),y
        jsr     aux_t0
        sta     n0
        jsr     rpush_p0        ; the container (p0 is its pointer, from peek)
        lda     n0
        pha
        lda     #0
        pha
        lda     rsp
        sec
        sbc     #4
        pha
@ll:    lda     3,s
        cmp     5,s
        beq     @done
        lda     1,s
        tax
        jsr     rget_x
        jsr     deref
        lda     3,s
        jsr     elem_t0
        jsr     push_p0
        jsr     deep_force_top
        jsr     pop_p0
        lda     3,s
        inc     a
        sta     3,s
        jmp     @ll
@attrs: ldy     #2
        lda     (t0),y
        jmi     @cycle
        ora     #$8000
        sta     (t0),y
        jsr     aux_t0
        sta     n0
        jsr     rpush_p0
        lda     n0
        pha
        lda     #0
        pha
        lda     rsp
        sec
        sbc     #4
        pha
@al:    lda     3,s
        cmp     5,s
        beq     @done
        lda     5,s
        sta     n0
        lda     1,s
        tax
        jsr     rget_x
        jsr     deref
        lda     3,s
        jsr     attr_value
        jsr     push_p0
        jsr     deep_force_top
        jsr     pop_p0
        lda     3,s
        inc     a
        sta     3,s
        jmp     @al
@done:  lda     1,s
        tax
        jsr     rget_x
        jsr     deref
        ldy     #2
        lda     (t0),y
        and     #$7FFF          ; unmark
        sta     (t0),y
        pla
        sta     rsp
        pla
        pla
@cycle: rts
