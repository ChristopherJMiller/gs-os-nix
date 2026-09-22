; ---------------------------------------------------------------------------
; data.s -- strings, lists and attribute sets for the VM. Included by vm.s
; and shares its zero page; 16-bit A/X/Y throughout unless bracketed.
;
; Scratch discipline (the one rule that matters here): anything that runs
; bytecode -- force_top, deep_force_top, do_call, values_equal -- may clobber
; EVERY scratch variable (t0..t6, n0, n1, ix, jx, kx, h, slen, sav, p0..p2).
; Loops that force or call keep their state on the CPU stack around the call.
; Routines that never run bytecode document what they clobber.
;
; Object layouts are HEAP.md's. Column offsets in an ATTRS of n entries:
;   hashes at 4 + 4i, values at 4 + 4n + 4i, names at 4 + 8n + 4i.
; ---------------------------------------------------------------------------

; --- small helpers ---------------------------------------------------------
; aux (low 16 bits) of the object at t0 / t1 -> A. Clobbers Y.
aux_t0: ldy     #1
        lda     (t0),y
        rts
aux_t1: ldy     #1
        lda     (t1),y
        rts

; hdr_obj: A = tag, t2 = aux (< 65536) -> the header at obj. Clobbers t3, Y.
hdr_obj:
        sta     t3
        lda     t2
        xba
        and     #$FF00
        ora     t3
        sta     (obj)
        lda     t2
        xba
        and     #$00FF
        ldy     #2
        sta     (obj),y
        rts

; round4: A -> (A + 3) & ~3
round4: clc
        adc     #3
        and     #$FFFC
        rts

; slot_addr: A = k (1 = top of the value stack) -> A = that slot's address.
; Clobbers t3.
slot_addr:
        asl
        asl
        sta     t3
        lda     vsp
        sec
        sbc     t3
        rts

; force_slot: force the value in the stack slot whose address is in t6, in
; place. Runs bytecode; preserves t6 (nothing else).
force_slot:
        lda     (t6)
        sta     p0
        ldy     #2
        lda     (t6),y
        sta     p0+2
        lda     t6
        pha
        jsr     push_p0
        jsr     force_top
        jsr     pop_p0
        pla
        sta     t6
        lda     p0
        sta     (t6)
        ldy     #2
        lda     p0+2
        sta     (t6),y
        rts

; deref_p1: p1 -> t1, same checks as deref.
deref_p1:
        lda     p1+2
        bpl     @bad
        lda     p1
        beq     @bad
        sta     t1
        rts
@bad:   lda     #E_BADNBC
        jmp     err_exit

; push_int: p2 (8 bytes little-endian) -> a new INT on the stack. Clobbers obj, p0.
push_int:
        jsr     new_int
        ldy     #4
        lda     p2
        sta     (obj),y
        ldy     #6
        lda     p2+2
        sta     (obj),y
        lda     #0
        ldy     #8
        sta     (obj),y
        ldy     #10
        sta     (obj),y
        jmp     push_p0

; push_int16: A (unsigned 16-bit) -> a new INT on the stack.
push_int16:
        sta     p2
        stz     p2+2
        bra     push_int

; push_static_str / push_bool via push_static (vm.s).

; Type checks on the object at t0.
need_list:
        lda     (t0)
        and     #$00FF
        cmp     #T_LIST
        jne     type_err
        rts
need_string:
        lda     (t0)
        and     #$00FF
        cmp     #T_STRING
        jne     type_err
        rts
need_attrs:
        lda     (t0)
        and     #$00FF
        cmp     #T_ATTRS
        jne     type_err
        rts
need_string_t1:
        lda     (t1)
        and     #$00FF
        cmp     #T_STRING
        jne     type_err
        rts

; force_pop_t0: force the top, pop it, deref into t0 (p0 holds the pointer).
force_pop_t0:
        jsr     force_top
        jsr     pop_p0
        jmp     deref

; force_two: force the top two values IN PLACE, then pop both: t1 = the top
; (b), t0 = the one below (a). Forcing runs bytecode, and force_top's in-place
; update itself writes t1, so a binary operation must force both operands
; before it reads either -- the first draft forced b into t1 and then forced
; a, and `a.x + 1` with a lazy a.x came out wrong.
force_two:
        jsr     force_top
        lda     #2
        jsr     slot_addr
        sta     t6
        jsr     force_slot
        jsr     pop_p0
        jsr     deref
        lda     t0
        sta     t1
        jsr     pop_p0
        jmp     deref

; --- strings --------------------------------------------------------------------------
; str_alloc: A = byte length -> obj/p0 a STRING of that length with a null
; context and a zeroed pad word; slen = length. Clobbers t2, t3, t4, Y.
str_alloc:
        sta     slen
        jsr     round4
        sta     t4
        clc
        adc     #8
        jsr     alloc
        lda     slen
        sta     t2
        lda     #T_STRING
        jsr     hdr_obj
        lda     t4
        clc
        adc     #4
        tay
        lda     #0
        sta     (obj),y         ; context pointer = null
        iny
        iny
        sta     (obj),y
        lda     t4
        beq     @done
        tay                     ; the last data word, so the pad bytes are 0
        lda     #0
        sta     (obj),y
        iny
        iny
        sta     (obj),y
@done:  rts

; copy_bytes: A = count, X = source, Y = destination. MVN; count 0 is fine.
; Leaves DBR = 0. Clobbers A, X, Y.
copy_bytes:
        cmp     #0
        beq     @done
        dec     a
        mvn     #$00, #$00
@done:  rts

; str_concat: STRING t0 ++ STRING t1 -> obj/p0. Clobbers n0, n1, t2..t4, slen.
str_concat:
        jsr     aux_t0
        sta     n0
        jsr     aux_t1
        sta     n1
        clc
        adc     n0
        jcs     nomem
        jsr     str_alloc
        lda     t0
        clc
        adc     #4
        tax
        lda     obj
        clc
        adc     #4
        tay
        lda     n0
        jsr     copy_bytes
        lda     t1
        clc
        adc     #4
        tax
        lda     obj
        clc
        adc     #4
        clc
        adc     n0
        tay
        lda     n1
        jmp     copy_bytes

; deref_follow: p0 -> t0, following INDIRECTs (a forced thunk that grew).
deref_follow:
        jsr     deref
@loop:  lda     (t0)
        and     #$00FF
        cmp     #T_INDIRECT
        bne     @done
        ldy     #4
        lda     (t0),y
        sta     p0
        iny
        iny
        lda     (t0),y
        sta     p0+2
        jsr     deref
        bra     @loop
@done:  rts

; str_eq: STRING t0 vs STRING t1 -> Z set if equal. Clobbers t2, t3, Y only,
; so callers may keep everything else live across it.
str_eq:
        jsr     aux_t0
        sta     t2
        jsr     aux_t1
        cmp     t2
        bne     @ne
        lda     t2
        jsr     round4
        sta     t3
        ldy     #4
@w:     lda     t3
        beq     @eq
        lda     (t0),y
        cmp     (t1),y
        bne     @ne
        iny
        iny
        dec     t3
        dec     t3
        bra     @w
@eq:    lda     #0
        rts
@ne:    lda     #1
        rts

; str_cmp: byte order of STRING t0 vs STRING t1 -> A = 0 less, 1 equal, 2 greater.
; Clobbers t2, t3, slen, Y.
str_cmp:
        jsr     aux_t0
        sta     t2
        jsr     aux_t1
        sta     t3
        cmp     t2
        bcs     @min_a
        sta     slen
        bra     @go
@min_a: lda     t2
        sta     slen
@go:    ldy     #4
@loop:  lda     slen
        beq     @lens
        dec     slen
        sep     #$20
        .a8
        lda     (t0),y
        cmp     (t1),y
        bcc     @less8
        bne     @greater8
        rep     #$20
        .a16
        iny
        bra     @loop
        .a8
@less8: rep     #$20
        .a16
        lda     #0
        rts
        .a8
@greater8:
        rep     #$20
        .a16
        lda     #2
        rts
@lens:  lda     t2
        cmp     t3
        bcc     @less
        beq     @equal
        lda     #2
        rts
@less:  lda     #0
        rts
@equal: lda     #1
        rts

; hash_t0: djb2 of the STRING at t0 -> h. Clobbers hc, hn, X, Y -- nothing
; a caller might hold a value in (the first draft used p2 and kx, and
; listToAttrs inserted garbage values with garbage counts).
hash_t0:
        lda     #5381
        sta     h
        stz     h+2
        jsr     aux_t0
        sta     hn
        ldy     #4
@loop:  lda     hn
        beq     @done
        dec     hn
        lda     h
        sta     hc
        lda     h+2
        sta     hc+2
        ldx     #5
@sh:    asl     h
        rol     h+2
        dex
        bne     @sh
        clc
        lda     h
        adc     hc
        sta     h
        lda     h+2
        adc     hc+2
        sta     h+2
        lda     (t0),y
        and     #$00FF
        clc
        adc     h
        sta     h
        lda     h+2
        adc     #0
        sta     h+2
        iny
        bra     @loop
@done:  rts

; --- lists ---------------------------------------------------------------------------
; list_alloc: A = count -> obj/p0 a LIST with that many (unset) slots. Clobbers t2, t3.
list_alloc:
        sta     t2
        asl
        asl
        clc
        adc     #4
        jsr     alloc
        lda     #T_LIST
        jmp     hdr_obj

; attrs_alloc: A = count -> obj/p0 an ATTRS with that many (unset) entries.
; Clobbers t2, t3, t4.
attrs_alloc:
        sta     t2
        asl
        asl
        sta     t4
        asl
        clc
        adc     t4
        clc
        adc     #4
        jsr     alloc
        lda     #T_ATTRS
        jmp     hdr_obj

; elem_t0: A = index -> p0 = element A of the LIST at t0. Clobbers Y.
elem_t0:
        asl
        asl
        clc
        adc     #4
        tay
        lda     (t0),y
        sta     p0
        iny
        iny
        lda     (t0),y
        sta     p0+2
        rts

; set_elem_obj: A = index, p0 = value -> slot A of the LIST at obj. Clobbers Y.
set_elem_obj:
        asl
        asl
        clc
        adc     #4
        tay
        lda     p0
        sta     (obj),y
        iny
        iny
        lda     p0+2
        sta     (obj),y
        rts

; LIST n: the n values on top of the stack, in push order, become a list.
op_list:
        jsr     fetch16
        sta     n0
        asl
        asl
        sta     t4              ; 4n
        lda     n0
        jsr     list_alloc
        lda     vsp
        sec
        sbc     t4
        sta     vsp             ; the n values are popped ...
        tax                     ; ... and copied from where they were
        lda     obj
        clc
        adc     #4
        tay
        lda     t4
        jsr     copy_bytes
        jsr     push_p0
        jmp     run

; CONCAT: a ++ b
op_concat:
        jsr     force_two       ; t0 = a, t1 = b
        jsr     need_list
        lda     (t1)
        and     #$00FF
        cmp     #T_LIST
        jne     type_err
        jsr     aux_t0
        sta     n0
        jsr     aux_t1
        sta     n1
        clc
        adc     n0
        jcs     nomem
        jsr     list_alloc
        lda     t0
        clc
        adc     #4
        tax
        lda     obj
        clc
        adc     #4
        tay
        lda     n0
        asl
        asl
        jsr     copy_bytes
        lda     t1
        clc
        adc     #4
        tax
        lda     n0
        asl
        asl
        clc
        adc     #4
        clc
        adc     obj
        tay
        lda     n1
        asl
        asl
        jsr     copy_bytes
        jsr     push_p0
        jmp     run

; --- attribute sets -------------------------------------------------------------------
; ATTRS n: n (name, value) pairs on the stack, names sorted, become a set.
op_attrs:
        jsr     fetch16
        sta     n0
        jsr     attrs_alloc
        lda     n0
        asl
        asl
        asl
        sta     t4              ; 8n
        lda     vsp
        sec
        sbc     t4
        sta     t5              ; the pairs start here; also the new vsp
        stz     ix
@loop:  lda     ix
        cmp     n0
        beq     @done
        asl
        asl
        asl
        clc
        adc     t5
        sta     t6              ; this pair: name at +0, value at +4
        lda     (t6)
        sta     p0
        ldy     #2
        lda     (t6),y
        sta     p0+2
        jsr     deref           ; t0 = the name STRING
        jsr     hash_t0
        jsr     attrs_put_ix    ; hash h, name p0, value from (t6)+4
        inc     ix
        bra     @loop
@done:  lda     t5
        sta     vsp
        jmp     push_obj_run    ; (deref above clobbered p0)

; attrs_put_ix: write entry ix of the ATTRS at obj (n0 entries): hash from h,
; name pointer from p0, value pointer from the pair at t6 (+4). Clobbers jx, Y.
attrs_put_ix:
        lda     ix
        asl
        asl
        sta     jx              ; 4i
        clc
        adc     #4
        tay
        lda     h
        sta     (obj),y
        iny
        iny
        lda     h+2
        sta     (obj),y
        lda     n0
        asl
        asl
        clc
        adc     jx
        clc
        adc     #4
        tay                     ; value column
        phy
        ldy     #4
        lda     (t6),y
        ply
        sta     (obj),y
        iny
        iny
        phy
        ldy     #6
        lda     (t6),y
        ply
        sta     (obj),y
        lda     n0
        asl
        asl
        asl
        clc
        adc     jx
        clc
        adc     #4
        tay                     ; name column
        lda     p0
        sta     (obj),y
        iny
        iny
        lda     p0+2
        sta     (obj),y
        rts

; attr_value: A = index -> p0 = value pointer of entry A in the ATTRS at t0
; with n0 entries. Clobbers t4, Y.
attr_value:
        asl
        asl
        sta     t4
        lda     n0
        asl
        asl
        clc
        adc     t4
        clc
        adc     #4
        tay
        lda     (t0),y
        sta     p0
        iny
        iny
        lda     (t0),y
        sta     p0+2
        rts

; attr_name: A = index -> p0 = name pointer of entry A in the ATTRS at t0
; with n0 entries. Clobbers t4, Y.
attr_name:
        asl
        asl
        sta     t4
        lda     n0
        asl
        asl
        asl
        clc
        adc     t4
        clc
        adc     #4
        tay
        lda     (t0),y
        sta     p0
        iny
        iny
        lda     (t0),y
        sta     p0+2
        rts

; find_attr: ATTRS at t0, STRING name at t1 -> C set and ix = index if found,
; C clear otherwise. n0 = the set's count afterwards. Clobbers h, hc, hn, t2,
; t3, t4, ix, p0, Y.
find_attr:
        lda     t0
        pha
        lda     t1
        sta     t0
        jsr     hash_t0
        pla
        sta     t0
        jsr     aux_t0
        sta     n0
        stz     ix
@scan:  lda     ix
        cmp     n0
        beq     @nf
        asl
        asl
        clc
        adc     #4
        tay
        lda     (t0),y
        cmp     h
        bne     @next
        iny
        iny
        lda     (t0),y
        cmp     h+2
        bne     @next
        lda     ix              ; hash matched: confirm the name
        jsr     attr_name
        lda     t0
        pha
        lda     p0
        sta     t0
        jsr     str_eq
        sta     t4              ; 0 = equal
        pla
        sta     t0
        lda     t4
        beq     @found
@next:  inc     ix
        bra     @scan
@found: sec
        rts
@nf:    clc
        rts

; SELECT: s k -> v
op_select:
        jsr     force_two       ; t0 = the set, t1 = the key
        jsr     need_attrs
        jsr     need_string_t1
        jsr     find_attr
        bcc     @missing
        lda     ix
        jsr     attr_value
        jsr     push_p0
        jmp     run
@missing:
        lda     #E_NOATTR
        jmp     err_exit

; TRY_SELECT: s k -> v | not-found (the null pointer)
op_try_select:
        jsr     force_two
        jsr     need_string_t1
        lda     (t0)
        and     #$00FF
        cmp     #T_ATTRS
        bne     @nf
        jsr     find_attr
        bcc     @nf
        lda     ix
        jsr     attr_value
        jsr     push_p0
        jmp     run
@nf:    stz     p0
        stz     p0+2
        jsr     push_p0
        jmp     run

op_jmp_if_notfound:
        jsr     fetch16
        sta     t2
        jsr     peek_p0
        lda     p0
        ora     p0+2
        bne     @no
        lda     pc
        clc
        adc     t2
        sta     pc
@no:    jmp     run

; HAS_ATTR: s k -> bool
op_has_attr:
        jsr     force_two
        jsr     need_string_t1
        lda     (t0)
        and     #$00FF
        cmp     #T_ATTRS
        bne     @no
        jsr     find_attr
        bcc     @no
        lda     #obj_true
        jsr     push_static
        jmp     run
@no:    lda     #obj_false
        jsr     push_static
        jmp     run

; UPDATE: a // b. Both sorted by name; a merge, b winning ties. Two passes:
; count the union, then fill, since the column offsets depend on the count.
op_update:
        jsr     force_two
        jsr     need_attrs
        lda     (t1)
        and     #$00FF
        cmp     #T_ATTRS
        jne     type_err
        lda     t1
        sta     t6              ; b
        lda     t0
        sta     sav             ; a
        jsr     merge_count     ; kx = |a u b|
        lda     kx
        jsr     attrs_alloc     ; obj
        jsr     merge_fill      ; clobbers p0
        jmp     push_obj_run

; Name pointer of entry A in the set at t0 -> t1 (young). Clobbers n0, t4, p0, Y.
name_addr_t0:
        pha
        jsr     aux_t0
        sta     n0
        pla
        jsr     attr_name
        lda     p0
        sta     t1
        rts

; merge_step: compare name ix of set `sav` with name jx of set `t6`.
; Returns A = 0 (a's name is less), 1 (equal), 2 (greater). Preserves ix, jx,
; kx, sav, t6. Clobbers t0, t1, n0, t4, t5, slen, p0, Y.
merge_step:
        lda     t6
        sta     t0
        lda     jx
        jsr     name_addr_t0    ; t1 = b's name
        lda     t1
        pha
        lda     sav
        sta     t0
        lda     ix
        jsr     name_addr_t0    ; t1 = a's name
        lda     t1
        sta     t0
        pla
        sta     t1
        jmp     str_cmp

; merge_count: kx = |a u b| for sets sav (a) and t6 (b).
merge_count:
        stz     ix
        stz     jx
        stz     kx
        lda     sav
        sta     t0
        jsr     aux_t0
        sta     n0
        pha                     ; |a|
        lda     t6
        sta     t0
        jsr     aux_t0
        pha                     ; |b|
@loop:  lda     1,s             ; |b|
        cmp     jx
        beq     @rest_a
        lda     3,s             ; |a|
        cmp     ix
        beq     @rest_b
        jsr     merge_step
        cmp     #1
        beq     @both
        bcc     @a
        inc     jx
        bra     @one
@a:     inc     ix
        bra     @one
@both:  inc     ix
        inc     jx
@one:   inc     kx
        bra     @loop
@rest_a:                        ; b exhausted: the rest of a
        lda     3,s
        sec
        sbc     ix
        clc
        adc     kx
        sta     kx
        bra     @done
@rest_b:
        lda     1,s
        sec
        sbc     jx
        clc
        adc     kx
        sta     kx
@done:  pla
        pla
        rts

; merge_fill: fill the ATTRS at obj (kx entries) from a (sav) and b (t6),
; b winning ties. Entry copy is copy_entry: from set t0 index A into obj at
; index n1 -- n1 counts the output.
merge_fill:
        stz     ix
        stz     jx
        stz     n1
        lda     sav
        sta     t0
        jsr     aux_t0
        pha                     ; |a|
        lda     t6
        sta     t0
        jsr     aux_t0
        pha                     ; |b|
@loop:  lda     1,s
        cmp     jx
        beq     @rest_a
        lda     3,s
        cmp     ix
        beq     @rest_b
        jsr     merge_step
        cmp     #1
        beq     @both
        bcc     @a
@b:     lda     t6
        sta     t0
        lda     jx
        jsr     copy_entry
        inc     jx
        bra     @loop
@a:     lda     sav
        sta     t0
        lda     ix
        jsr     copy_entry
        inc     ix
        bra     @loop
@both:  inc     ix
        bra     @b
@rest_a:
        lda     3,s
        cmp     ix
        beq     @done
        lda     sav
        sta     t0
        lda     ix
        jsr     copy_entry
        inc     ix
        bra     @rest_a
@rest_b:
        lda     1,s
        cmp     jx
        beq     @done
        lda     t6
        sta     t0
        lda     jx
        jsr     copy_entry
        inc     jx
        bra     @rest_b
@done:  pla
        pla
        rts

; copy_entry: entry A of the ATTRS at t0 -> entry n1 of the ATTRS at obj (kx
; entries), then n1++. Clobbers n0, t4, t5, p0, Y.
copy_entry:
        sta     t5              ; source index
        jsr     aux_t0
        sta     n0
        ; hash
        lda     t5
        asl
        asl
        clc
        adc     #4
        tay
        lda     (t0),y
        pha
        iny
        iny
        lda     (t0),y
        pha
        lda     n1
        asl
        asl
        clc
        adc     #4
        tay
        pla
        iny
        iny
        sta     (obj),y
        dey
        dey
        pla
        sta     (obj),y
        ; value
        lda     t5
        jsr     attr_value
        lda     kx
        asl
        asl
        sta     t4
        lda     n1
        asl
        asl
        clc
        adc     t4
        clc
        adc     #4
        tay
        lda     p0
        sta     (obj),y
        iny
        iny
        lda     p0+2
        sta     (obj),y
        ; name
        lda     t5
        jsr     attr_name
        lda     kx
        asl
        asl
        asl
        sta     t4
        lda     n1
        asl
        asl
        clc
        adc     t4
        clc
        adc     #4
        tay
        lda     p0
        sta     (obj),y
        iny
        iny
        lda     p0+2
        sta     (obj),y
        inc     n1
        rts

; --- interpolation ---------------------------------------------------------------
; INTERPOLATE n: the n parts on the stack, forced, must be STRINGs; concatenate.
op_interpolate:
        jsr     fetch16
        sta     n0
        stz     ix
@force: lda     ix
        cmp     n0
        beq     @sum
        lda     n0
        sec
        sbc     ix
        jsr     slot_addr
        sta     t6
        lda     ix
        pha
        lda     n0
        pha
        jsr     force_slot
        pla
        sta     n0
        pla
        sta     ix
        inc     ix
        bra     @force
@sum:   stz     slen
        stz     ix
@len:   lda     ix
        cmp     n0
        beq     @alloc
        lda     n0
        sec
        sbc     ix
        jsr     slot_addr
        sta     t6
        lda     (t6)
        sta     p0
        ldy     #2
        lda     (t6),y
        sta     p0+2
        jsr     deref
        jsr     need_string
        jsr     aux_t0
        clc
        adc     slen
        jcs     nomem
        sta     slen
        inc     ix
        bra     @len
@alloc: lda     slen
        jsr     str_alloc       ; obj; slen preserved
        stz     ix
        stz     t5              ; bytes written so far
@copy:  lda     ix
        cmp     n0
        beq     @done
        lda     n0
        sec
        sbc     ix
        jsr     slot_addr
        sta     t6
        lda     (t6)
        sta     t0              ; young, already checked
        jsr     aux_t0
        sta     n1
        lda     t0
        clc
        adc     #4
        tax
        lda     obj
        clc
        adc     #4
        clc
        adc     t5
        tay
        lda     n1
        jsr     copy_bytes
        lda     t5
        clc
        adc     n1
        sta     t5
        inc     ix
        bra     @copy
@done:  lda     n0
        asl
        asl
        sta     t4
        lda     vsp
        sec
        sbc     t4
        sta     vsp
        jsr     push_p0
        jmp     run

