; ---------------------------------------------------------------------------
; prims.s -- the primops (BYTECODE.md "Primops"). Included by vm.s.
;
; PRIM id: the arguments are on the value stack, first argument deepest,
; unforced. Each primop pops what it takes, forces what it needs, pushes one
; value and `jmp run`s. The table order is the id order; checks.nbc-opcodes
; keeps it in step with the generator and the document.
;
; Loops that call back into Nix (map, filter, foldl', any, all, ...) keep
; their state on the CPU stack, addressed stack-relative, because do_call
; clobbers every zero-page scratch variable.
; ---------------------------------------------------------------------------

NUMBUF          = $0BC0         ; 8-byte magnitude for decimal conversion
DIGITS          = $0BD0         ; up to 20 digits and a sign (past the catch frames)

op_prim:
        jsr     fetch8
        cmp     #PRIM_COUNT
        jcs     op_bad
        asl
        tax
        jmp     (primtable,x)

primtable:
        .word   prim_length, prim_head, prim_tail, prim_elemat             ; $00-$03
        .word   prim_map, prim_filter, prim_foldl, prim_concatlists        ; $04-$07
        .word   prim_genlist, prim_elem, prim_any, prim_all                ; $08-$0B
        .word   prim_concatmap, prim_attrnames, prim_attrvalues            ; $0C-$0E
        .word   prim_hasattr, prim_getattr, prim_stringlength              ; $0F-$11
        .word   prim_substring, prim_tostring, prim_concatstringssep       ; $12-$14
        .word   prim_throw, prim_abort, prim_seq, prim_deepseq             ; $15-$18
        .word   prim_typeof, prim_isint, prim_isbool, prim_isstring        ; $19-$1C
        .word   prim_isnull, prim_islist, prim_isattrs, prim_isfunction    ; $1D-$20
        .word   op_add, op_sub, op_mul, op_div, op_lt                      ; $21-$25
        .word   prim_bitand, prim_bitor, prim_bitxor                       ; $26-$28
        .word   prim_listtoattrs, prim_intersectattrs, prim_removeattrs    ; $29-$2B
        .word   prim_partition, prim_basenameof, prim_dirof                ; $2C-$2E
        .word   prim_tryeval                                              ; $2F

; --- shared -------------------------------------------------------------------------
index_err:
        lda     #E_INDEX
        jmp     err_exit

; pop_int16: force and pop an INT; A = its low word. Words 1..3 must be zero
; (indices and lengths are small) or it is an index error. t0 = the INT.
pop_index:
        jsr     force_pop_t0
        jsr     need_int
        ldy     #6
        lda     (t0),y
        bne     index_err
        ldy     #8
        lda     (t0),y
        bne     index_err
        ldy     #10
        lda     (t0),y
        bne     index_err
        ldy     #4
        lda     (t0),y
        rts

; make_apply: p1 = f, p2 = x -> p0 = a THUNK computing (f x) when forced. The
; body is apply_code below: a chunk the VM carries, so map and genList can be
; lazy without the generator's help. Clobbers obj, t6, Y.
make_apply:
        lda     #16
        jsr     alloc           ; env LIST(3): [0] null, [1] f, [2] x
        lda     #(T_LIST | (3 << 8))
        sta     (obj)
        lda     #0
        ldy     #2
        sta     (obj),y
        ldy     #4
        sta     (obj),y
        iny
        iny
        sta     (obj),y
        ldy     #8
        lda     p1
        sta     (obj),y
        iny
        iny
        lda     p1+2
        sta     (obj),y
        ldy     #12
        lda     p2
        sta     (obj),y
        iny
        iny
        lda     p2+2
        sta     (obj),y
        lda     obj
        sta     t6
        lda     #12
        jsr     alloc
        lda     #T_THUNK
        sta     (obj)
        lda     #0
        ldy     #2
        sta     (obj),y
        ldy     #4
        lda     #apply_code
        sta     (obj),y
        ldy     #6
        lda     #0
        sta     (obj),y
        ldy     #8
        lda     t6
        sta     (obj),y
        ldy     #10
        lda     #$8000
        sta     (obj),y
        rts

apply_code:
        .byte   OP_ENV_GET, 0, 0, 1, 0
        .byte   OP_ENV_GET, 0, 0, 2, 0
        .byte   OP_CALL
        .byte   OP_RETURN

; call_with: the function pointer is in p0, the argument in p1; pushes both and
; calls; leaves the call's value on top of the stack. Runs bytecode.
call_with:
        jsr     push_p0
        lda     p1
        sta     p0
        lda     p1+2
        sta     p0+2
        jsr     push_p0
        jmp     do_call

; --- lists --------------------------------------------------------------------------
prim_length:
        jsr     force_pop_t0
        jsr     need_list
        jsr     aux_t0
        jsr     push_int16
        jmp     run

prim_head:
        jsr     force_pop_t0
        jsr     need_list
        jsr     aux_t0
        jeq     index_err
        lda     #0
        jsr     elem_t0
        jsr     push_p0
        jmp     run

prim_tail:
        jsr     force_pop_t0
        jsr     need_list
        jsr     aux_t0
        jeq     index_err
        dec     a
        sta     n0
        jsr     list_alloc
        lda     t0
        clc
        adc     #8
        tax
        lda     obj
        clc
        adc     #4
        tay
        lda     n0
        asl
        asl
        jsr     copy_bytes
        jsr     push_p0
        jmp     run

prim_elemat:                    ; list i
        jsr     pop_index
        pha
        jsr     force_pop_t0
        jsr     need_list
        jsr     aux_t0
        sta     n0
        pla
        cmp     n0
        jcs     index_err
        jsr     elem_t0
        jsr     push_p0
        jmp     run

; push_obj_run: push the object at obj as a young pointer, then dispatch.
push_obj_run:
        lda     obj
        sta     p0
        lda     #$8000
        sta     p0+2
        jsr     push_p0
        jmp     run

; map f list -> a list of (f x) thunks
prim_map:
        jsr     force_pop_t0
        jsr     need_list
        jsr     aux_t0
        sta     n0
        jsr     pop_p0          ; f
        lda     p0
        sta     p1
        lda     p0+2
        sta     p1+2
        lda     n0
        jsr     list_alloc
        lda     obj
        sta     sav
        stz     ix
@loop:  lda     ix
        cmp     n0
        beq     @done
        jsr     elem_t0
        lda     p0
        sta     p2
        lda     p0+2
        sta     p2+2
        jsr     make_apply      ; p0 = thunk (f x); clobbers obj, t6
        lda     sav
        sta     obj
        lda     ix
        jsr     set_elem_obj
        inc     ix
        bra     @loop
@done:  lda     sav
        sta     p0
        lda     #$8000
        sta     p0+2
        jsr     push_p0
        jmp     run

; genList f n -> [ (f 0) ... (f (n-1)) ], thunks
prim_genlist:
        jsr     pop_index
        sta     n0
        jsr     pop_p0          ; f
        lda     p0
        sta     p1
        lda     p0+2
        sta     p1+2
        lda     n0
        jsr     list_alloc
        lda     obj
        sta     sav
        stz     ix
@loop:  lda     ix
        cmp     n0
        beq     @done
        jsr     new_int         ; the index as an INT (clobbers obj, p0)
        ldy     #4
        lda     ix
        sta     (obj),y
        lda     #0
        ldy     #6
        sta     (obj),y
        ldy     #8
        sta     (obj),y
        ldy     #10
        sta     (obj),y
        lda     p0
        sta     p2
        lda     p0+2
        sta     p2+2
        jsr     make_apply
        lda     sav
        sta     obj
        lda     ix
        jsr     set_elem_obj
        inc     ix
        bra     @loop
@done:  lda     sav
        sta     p0
        lda     #$8000
        sta     p0+2
        jsr     push_p0
        jmp     run

; --- frame helpers for the loops below ----------------------------------------------
; Every loop that calls back into Nix keeps its objects on the root stack and
; its counters on the CPU stack, with rbase at 1,s. robj_k: the object at
; entry k of the current frame -> p0 and t0 (dereferenced). Called as a
; subroutine, so the frame is 2 bytes deeper: rbase is at 3,s here.
robj_0: lda     3,s
        tax
        bra     robj_x
robj_1: lda     3,s
        clc
        adc     #4
        tax
        bra     robj_x
robj_2: lda     3,s
        clc
        adc     #8
        tax
        bra     robj_x
robj_3: lda     3,s
        clc
        adc     #12
        tax
robj_x: jsr     rget_x
        jmp     deref

; rpush_obj: push the young object at obj as a root.
rpush_obj:
        lda     obj
        sta     p0
        lda     #$8000
        sta     p0+2
        jmp     rpush_p0

; filter f list. Roots: list, f, result. CPU: kept 7,s  i 5,s  n 3,s  rbase 1,s
prim_filter:
        jsr     force_pop_t0
        jsr     need_list
        jsr     aux_t0
        sta     n0
        jsr     rpush_p0        ; list
        jsr     pop_p0
        jsr     rpush_p0        ; f
        lda     n0
        jsr     list_alloc
        jsr     rpush_obj       ; result
        lda     #0
        pha                     ; kept
        lda     n0
        pha
        lda     #0
        pha                     ; i
        lda     rsp
        sec
        sbc     #12
        pha
@loop:  lda     3,s
        cmp     5,s
        beq     @done
        jsr     robj_0          ; the list
        lda     3,s
        jsr     elem_t0
        lda     p0
        sta     p1
        lda     p0+2
        sta     p1+2
        lda     1,s
        clc
        adc     #4
        tax
        jsr     rget_x          ; f
        jsr     call_with
        jsr     force_pop_t0
        jsr     need_bool
        ldy     #4
        lda     (t0),y
        and     #$00FF
        beq     @skip
        jsr     robj_2          ; the result list -> obj (before p0 is the element)
        lda     t0
        sta     obj
        jsr     robj_0
        lda     3,s
        jsr     elem_t0
        lda     7,s
        jsr     set_elem_obj
        lda     7,s
        inc     a
        sta     7,s
@skip:  lda     3,s
        inc     a
        sta     3,s
        bra     @loop
@done:  jsr     robj_2
        lda     t0
        sta     obj
        lda     7,s
        sta     t2
        lda     #T_LIST
        jsr     hdr_obj         ; aux = kept
        lda     1,s
        sta     rsp
        tsc
        clc
        adc     #8
        tcs
        jmp     push_obj_run

; foldl' f z list: the accumulator lives on the value stack.
; Frame: i 1,s  n 3,s  f.hi 5,s  f.lo 7,s  list 9,s
; foldl' f z list: the accumulator lives on the value stack. Roots: list, f.
; CPU: n 5,s  i 3,s  rbase 1,s
prim_foldl:                     ; f z list, list on top
        jsr     force_pop_t0
        jsr     need_list
        jsr     aux_t0
        sta     n0
        jsr     rpush_p0        ; list
        jsr     pop_p0          ; z
        lda     p0
        sta     p2
        lda     p0+2
        sta     p2+2
        jsr     pop_p0          ; f
        jsr     rpush_p0
        lda     n0
        pha
        lda     #0
        pha
        lda     rsp
        sec
        sbc     #8
        pha
        lda     p2
        sta     p0
        lda     p2+2
        sta     p0+2
        jsr     push_p0         ; the accumulator
@loop:  lda     3,s
        cmp     5,s
        beq     @done
        ; g = f acc
        jsr     pop_p0          ; acc
        lda     p0
        sta     p1
        lda     p0+2
        sta     p1+2
        lda     1,s
        clc
        adc     #4
        tax
        jsr     rget_x          ; f
        jsr     call_with       ; g on top
        ; acc' = g x, forced (strict accumulator)
        jsr     robj_0
        lda     3,s
        jsr     elem_t0
        jsr     push_p0         ; x above g
        jsr     do_call
        jsr     force_top
        lda     3,s
        inc     a
        sta     3,s
        bra     @loop
@done:  pla
        sta     rsp
        pla
        pla
        jmp     run             ; the accumulator is the result

; concatLists [ [..] [..] ]: force each element, sum, copy. Two passes; the
; state is kept on the CPU stack around the forces.
; Roots: outer. CPU: total 7,s  i 5,s  n 3,s  rbase 1,s
prim_concatlists:
        jsr     force_pop_t0
        jsr     need_list
        jsr     aux_t0
        sta     n0
        jsr     rpush_p0
        lda     #0
        pha                     ; total
        lda     n0
        pha
        lda     #0
        pha                     ; i
        lda     rsp
        sec
        sbc     #4
        pha
@count: lda     3,s
        cmp     5,s
        beq     @alloc
        jsr     robj_0
        lda     3,s
        jsr     elem_t0
        jsr     push_p0
        jsr     force_top
        jsr     pop_p0
        jsr     deref
        jsr     need_list
        jsr     aux_t0
        clc
        adc     7,s
        jcs     nomem
        sta     7,s
        lda     3,s
        inc     a
        sta     3,s
        bra     @count
@alloc: lda     7,s
        jsr     list_alloc      ; obj = result (no bytecode from here on)
        lda     #0
        sta     3,s             ; i = 0
        sta     7,s             ; bytes written
@copy:  lda     3,s
        cmp     5,s
        beq     @done
        jsr     robj_0
        lda     3,s
        jsr     elem_t0
        jsr     deref_follow    ; already forced; maybe through an INDIRECT
        jsr     aux_t0
        asl
        asl
        sta     t4              ; bytes of this inner list
        lda     t0
        clc
        adc     #4
        tax
        lda     obj
        clc
        adc     #4
        clc
        adc     7,s
        tay
        lda     t4
        jsr     copy_bytes
        lda     7,s
        clc
        adc     t4
        sta     7,s
        lda     3,s
        inc     a
        sta     3,s
        bra     @copy
@done:  pla
        sta     rsp
        tsc
        clc
        adc     #6
        tcs
        jmp     push_obj_run    ; (elem_t0 above clobbered p0)

; concatMap f list = concatLists (map f list), the map strict.
; concatMap f list = concatLists (map f list), the map strict.
; Roots: list, f, mapped. CPU: n 5,s  i 3,s  rbase 1,s
prim_concatmap:
        jsr     force_pop_t0
        jsr     need_list
        jsr     aux_t0
        sta     n0
        jsr     rpush_p0        ; list
        jsr     pop_p0          ; f
        jsr     rpush_p0
        lda     n0
        jsr     list_alloc      ; the mapped list
        jsr     rpush_obj
        lda     n0
        pha
        lda     #0
        pha
        lda     rsp
        sec
        sbc     #12
        pha
@loop:  lda     3,s
        cmp     5,s
        beq     @done
        jsr     robj_0
        lda     3,s
        jsr     elem_t0
        lda     p0
        sta     p1
        lda     p0+2
        sta     p1+2
        lda     1,s
        clc
        adc     #4
        tax
        jsr     rget_x          ; f
        jsr     call_with
        jsr     force_top
        jsr     pop_p0
        lda     p0
        sta     p1
        lda     p0+2
        sta     p1+2
        jsr     robj_2          ; mapped
        lda     t0
        sta     obj
        lda     p1
        sta     p0
        lda     p1+2
        sta     p0+2
        lda     3,s
        jsr     set_elem_obj
        lda     3,s
        inc     a
        sta     3,s
        bra     @loop
@done:  jsr     robj_2
        pla
        sta     rsp
        pla
        pla
        jsr     push_p0         ; the mapped list
        jmp     prim_concatlists

; elem x list. Roots: list, x. CPU: n 5,s  i 3,s  rbase 1,s
prim_elem:
        jsr     force_pop_t0
        jsr     need_list
        jsr     aux_t0
        sta     n0
        jsr     rpush_p0        ; list
        jsr     pop_p0          ; x
        jsr     rpush_p0
        lda     n0
        pha
        lda     #0
        pha
        lda     rsp
        sec
        sbc     #8
        pha
@loop:  lda     3,s
        cmp     5,s
        beq     @no
        jsr     robj_0
        lda     3,s
        jsr     elem_t0
        lda     p0
        sta     p1
        lda     p0+2
        sta     p1+2
        lda     1,s
        clc
        adc     #4
        tax
        jsr     rget_x          ; x
        jsr     values_equal
        beq     @yes
        lda     3,s
        inc     a
        sta     3,s
        bra     @loop
@yes:   pla
        sta     rsp
        pla
        pla
        lda     #obj_true
        jsr     push_static
        jmp     run
@no:    pla
        sta     rsp
        pla
        pla
        lda     #obj_false
        jsr     push_static
        jmp     run

; any f list / all f list. Roots: list, f. CPU: want 7,s  n 5,s  i 3,s  rbase 1,s
prim_any:
        lda     #1
        bra     any_all
prim_all:
        lda     #0
any_all:
        pha                     ; the boolean that decides early
        jsr     force_pop_t0
        jsr     need_list
        jsr     aux_t0
        sta     n0
        jsr     rpush_p0        ; list
        jsr     pop_p0          ; f
        jsr     rpush_p0
        lda     n0
        pha
        lda     #0
        pha
        lda     rsp
        sec
        sbc     #8
        pha
@loop:  lda     3,s
        cmp     5,s
        beq     @exhausted
        jsr     robj_0
        lda     3,s
        jsr     elem_t0
        lda     p0
        sta     p1
        lda     p0+2
        sta     p1+2
        lda     1,s
        clc
        adc     #4
        tax
        jsr     rget_x          ; f
        jsr     call_with
        jsr     force_pop_t0
        jsr     need_bool
        ldy     #4
        lda     (t0),y
        and     #$00FF
        cmp     7,s
        beq     @decided
        lda     3,s
        inc     a
        sta     3,s
        bra     @loop
@decided:
        lda     7,s             ; any found true -> true; all found false -> false
        bra     @result
@exhausted:
        lda     7,s
        eor     #1              ; any: false; all: true
@result:
        sta     t2              ; nothing below runs bytecode
        pla
        sta     rsp
        tsc
        clc
        adc     #6              ; i, n, want
        tcs
        lda     t2
        beq     @false
        lda     #obj_true
        jsr     push_static
        jmp     run
@false: lda     #obj_false
        jsr     push_static
        jmp     run

; --- attribute sets -------------------------------------------------------------------
prim_attrnames:
        jsr     force_pop_t0
        jsr     need_attrs
        jsr     aux_t0
        sta     n0
        jsr     list_alloc
        lda     n0
        asl
        asl
        asl
        clc
        adc     #4
        clc
        adc     t0
        tax                     ; the name column
        lda     obj
        clc
        adc     #4
        tay
        lda     n0
        asl
        asl
        jsr     copy_bytes
        jsr     push_p0
        jmp     run

prim_attrvalues:
        jsr     force_pop_t0
        jsr     need_attrs
        jsr     aux_t0
        sta     n0
        jsr     list_alloc
        lda     n0
        asl
        asl
        clc
        adc     #4
        clc
        adc     t0
        tax                     ; the value column
        lda     obj
        clc
        adc     #4
        tay
        lda     n0
        asl
        asl
        jsr     copy_bytes
        jsr     push_p0
        jmp     run

; hasAttr name set / getAttr name set. Nix forces the NAME first, so a
; throwing name is caught before a non-set second argument is a type error.
force_name_then_set:
        lda     #2
        jsr     slot_addr
        sta     t6
        jsr     force_slot      ; the name, in place
        jsr     force_pop_t0
        jsr     need_attrs
        lda     t0
        pha
        jsr     pop_p0
        jsr     deref
        jsr     need_string
        lda     t0
        sta     t1
        pla
        sta     t0
        rts

prim_hasattr:
        jsr     force_name_then_set
        jsr     find_attr
        bcc     @no
        lda     #obj_true
        jsr     push_static
        jmp     run
@no:    lda     #obj_false
        jsr     push_static
        jmp     run

prim_getattr:
        jsr     force_name_then_set
        jsr     find_attr
        bcc     @missing
        lda     ix
        jsr     attr_value
        jsr     push_p0
        jmp     run
@missing:
        lda     #E_NOATTR
        jmp     err_exit

; intersectAttrs a b: names of a that are in b, with b's values. b is walked
; for each name of a; sizes are small. Two passes, then fill.
prim_intersectattrs:
        jsr     force_two       ; t0 = a, t1 = b
        jsr     need_attrs
        lda     (t1)
        and     #$00FF
        cmp     #T_ATTRS
        jne     type_err
        lda     t1
        sta     t6              ; b
        lda     t0
        sta     sav             ; a
        jsr     aux_t0
        sta     n1              ; |a|
        stz     ix
        stz     kx
@count: lda     ix
        cmp     n1
        beq     @alloc
        jsr     a_name_in_b
        bcc     @c_next
        inc     kx
@c_next:
        inc     ix
        bra     @count
@alloc: lda     kx
        jsr     attrs_alloc
        stz     ix
        stz     n1
        lda     sav
        sta     t0
        jsr     aux_t0
        pha                     ; |a| (copy_entry clobbers t4/t5)
@fill:  lda     ix
        cmp     1,s
        beq     @done
        jsr     a_name_in_b     ; jx = index in b
        bcc     @f_next
        lda     t6
        sta     t0
        lda     jx
        jsr     copy_entry      ; entry n1++ <- b[jx]
@f_next:
        inc     ix
        bra     @fill
@done:  pla
        jmp     push_obj_run

; a_name_in_b: is name ix of set `sav` in set `t6`? C set and jx = its index
; there. Preserves ix, kx, sav, t6, n1.
a_name_in_b:
        lda     sav
        sta     t0
        jsr     aux_t0
        sta     n0
        lda     ix
        jsr     attr_name
        lda     p0
        sta     t1
        lda     t6
        sta     t0
        lda     ix
        pha
        lda     n1
        pha
        lda     kx
        pha
        jsr     find_attr       ; ix := index in b; clobbers kx too
        lda     ix
        sta     jx
        pla
        sta     kx
        pla
        sta     n1
        pla
        sta     ix
        rts

; removeAttrs set [ names ]: keep entries whose name is not in the list.
; The list's elements are forced first; then two passes.
; removeAttrs set [ names ]. Roots while forcing the names: set, list.
; CPU: n 5,s  i 3,s  rbase 1,s. Then two passes with no bytecode.
prim_removeattrs:
        jsr     force_two       ; t0 = the set, t1 = the names list
        jsr     need_attrs
        lda     (t1)
        and     #$00FF
        cmp     #T_LIST
        jne     type_err
        jsr     aux_t1
        sta     n1
        lda     t0
        sta     p0
        lda     #$8000
        sta     p0+2
        jsr     rpush_p0        ; set
        lda     t1
        sta     p0
        jsr     rpush_p0        ; list
        lda     n1
        pha
        lda     #0
        pha
        lda     rsp
        sec
        sbc     #8
        pha
@force: lda     3,s             ; force every name (runs bytecode: frame-based)
        cmp     5,s
        beq     @forced
        jsr     robj_1          ; the list
        lda     3,s
        jsr     elem_t0
        jsr     push_p0
        jsr     force_top
        jsr     pop_p0
        jsr     deref
        jsr     need_string
        lda     3,s
        inc     a
        sta     3,s
        bra     @force
@forced:
        jsr     robj_1
        lda     t0
        sta     t6              ; names list
        jsr     robj_0
        lda     t0
        sta     sav             ; the set
        pla
        sta     rsp
        pla
        pla
        jsr     aux_t0
        pha                     ; |set| at 1,s
        stz     ix
        stz     kx
@count: lda     ix
        cmp     1,s
        beq     @alloc
        jsr     name_in_list
        bcs     @c_next
        inc     kx
@c_next:
        inc     ix
        bra     @count
@alloc: lda     kx
        jsr     attrs_alloc
        stz     ix
        stz     n1
@fill:  lda     ix
        cmp     1,s
        beq     @done
        jsr     name_in_list
        bcs     @f_next
        lda     sav
        sta     t0
        lda     ix
        jsr     copy_entry
@f_next:
        inc     ix
        bra     @fill
@done:  pla
        jmp     push_obj_run

; name_in_list: is name ix of set `sav` among the STRINGs of LIST t6? C set if
; so. Preserves ix, kx, sav, t6, n1. Elements were forced by the caller and
; may be INDIRECTs to their strings.
name_in_list:
        lda     sav
        sta     t0
        jsr     aux_t0
        sta     n0
        lda     ix
        jsr     attr_name
        lda     p0
        sta     t1              ; the name
        lda     t6
        sta     t0
        jsr     aux_t0
        sta     n0
        stz     jx
@loop:  lda     jx
        cmp     n0
        beq     @no
        lda     t6
        sta     t0
        lda     jx
        jsr     elem_t0
        jsr     deref_follow    ; t0 = a list element (forced STRING)
        jsr     str_eq
        beq     @yes
        inc     jx
        bra     @loop
@yes:   sec
        rts
@no:    clc
        rts

; listToAttrs [ { name; value; } ... ]: first occurrence of a name wins.
; Built by insertion into a capacity-n set kept sorted; compacted at the end
; if duplicates were dropped.
; Roots: list, set, and while an element is being taken apart, the element
; and its name (rbase+8, rbase+12). CPU: cnt 7,s  n 5,s  i 3,s  rbase 1,s
prim_listtoattrs:
        jsr     force_pop_t0
        jsr     need_list
        jsr     aux_t0
        sta     n0
        jsr     rpush_p0        ; list
        lda     n0
        jsr     attrs_alloc     ; capacity n0; column offsets use n0
        jsr     rpush_obj       ; set
        lda     #0
        pha                     ; entries so far
        lda     n0
        pha
        lda     #0
        pha                     ; i
        lda     rsp
        sec
        sbc     #8
        pha
@loop:  lda     3,s
        cmp     5,s
        jeq     @done
        jsr     robj_0
        lda     3,s
        jsr     elem_t0
        jsr     push_p0
        jsr     force_top
        jsr     pop_p0
        jsr     deref
        jsr     need_attrs
        jsr     rpush_p0        ; the element set, rbase+8
        lda     #str_name
        sta     t1
        jsr     find_attr
        bcc     @bad
        lda     ix
        jsr     attr_value
        jsr     push_p0
        jsr     force_top
        jsr     pop_p0
        jsr     deref
        jsr     need_string
        jsr     rpush_p0        ; the name STRING, rbase+12
        jsr     robj_2          ; the element set again (maybe moved)
        lda     #str_value
        sta     t1
        jsr     find_attr
        bcc     @bad
        lda     ix
        jsr     attr_value      ; p0 = the value, unforced
        lda     p0
        sta     p2
        lda     p0+2
        sta     p2+2
        jsr     robj_3          ; the name
        lda     t0
        sta     t5
        jsr     robj_1          ; the set
        lda     t0
        sta     obj
        lda     5,s
        sta     n0
        lda     7,s
        sta     kx
        jsr     insert_entry    ; kx updated
        lda     kx
        sta     7,s
        lda     1,s
        clc
        adc     #8
        sta     rsp             ; drop element and name
        lda     3,s
        inc     a
        sta     3,s
        jmp     @loop
@bad:   lda     #E_NOATTR
        jmp     err_exit
@done:  jsr     robj_1
        lda     t0
        sta     obj
        lda     7,s
        sta     kx
        lda     5,s
        cmp     kx
        beq     @exact
        ; duplicates were dropped: rebuild with exactly kx entries
        lda     obj
        sta     sav
        lda     kx
        jsr     attrs_alloc
        stz     ix
        stz     n1
@compact:
        lda     ix
        cmp     kx
        beq     @exact
        lda     sav
        sta     t0
        lda     ix
        jsr     copy_entry      ; the source's aux is its capacity: right offsets
        inc     ix
        bra     @compact
@exact: pla
        sta     rsp
        tsc
        clc
        adc     #6
        tcs
        jmp     push_obj_run

; insert_entry: name STRING t5, value p2 -> into the ATTRS at obj (capacity
; n0, kx entries so far, sorted). A duplicate name is dropped. Clobbers
; t0, t1, t4, h, p0, p1, jx, ix, slen, Y.
insert_entry:
        stz     jx
@find:  lda     jx
        cmp     kx
        beq     @append
        lda     obj
        sta     t0
        lda     jx
        jsr     attr_name       ; uses n0 = capacity
        lda     p0
        sta     t0
        lda     t5
        sta     t1
        jsr     str_cmp         ; existing vs new
        cmp     #1
        beq     @dup
        bcs     @shift          ; existing > new: insert here
        inc     jx
        bra     @find
@dup:   rts
@shift: ; move entries jx..kx-1 up by one, from the top
        lda     kx
        sta     ix
@sh:    lda     ix
        cmp     jx
        beq     @put
        dec     ix
        lda     obj
        sta     t0
        lda     ix
        jsr     entry_move_up
        bra     @sh
@append:
        lda     kx
        sta     jx
@put:   lda     t5
        sta     t0
        jsr     hash_t0
        lda     t5
        sta     p1
        lda     #$8000
        sta     p1+2
        lda     jx
        sta     ix
        jsr     attrs_set_entry
        inc     kx
        rts

; entry_move_up: entry A of the ATTRS at t0 (n0 capacity) -> entry A+1.
; Clobbers t4, Y, p0.
entry_move_up:
        pha
        asl
        asl
        clc
        adc     #4
        tay                     ; hash A: lo at y, hi at y+2
        lda     (t0),y
        pha
        iny
        iny
        lda     (t0),y
        iny
        iny
        iny
        iny
        sta     (t0),y          ; hi of A+1 at y+6
        dey
        dey
        pla
        sta     (t0),y          ; lo of A+1 at y+4
        lda     1,s
        jsr     attr_value      ; p0 = value A  (t0's set)
        lda     1,s
        inc     a
        jsr     value_slot_y    ; Y = value slot A+1
        lda     p0
        sta     (t0),y
        iny
        iny
        lda     p0+2
        sta     (t0),y
        lda     1,s
        jsr     attr_name
        lda     1,s
        inc     a
        jsr     name_slot_y
        lda     p0
        sta     (t0),y
        iny
        iny
        lda     p0+2
        sta     (t0),y
        pla
        rts

value_slot_y:                   ; A = index -> Y = offset of value slot (n0 entries)
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
        rts
name_slot_y:
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
        rts

; attrs_set_entry: entry ix of the ATTRS at obj (n0 entries) <- hash h, name
; p1, value p2. Clobbers t4, Y, t0.
attrs_set_entry:
        lda     obj
        sta     t0
        lda     ix
        asl
        asl
        clc
        adc     #4
        tay
        lda     h
        sta     (t0),y
        iny
        iny
        lda     h+2
        sta     (t0),y
        lda     ix
        jsr     value_slot_y
        lda     p2
        sta     (t0),y
        iny
        iny
        lda     p2+2
        sta     (t0),y
        lda     ix
        jsr     name_slot_y
        lda     p1
        sta     (t0),y
        iny
        iny
        lda     p1+2
        sta     (t0),y
        rts

; partition f list -> { right = [...]; wrong = [...]; }
; Roots: list, f, right, wrong. CPU: nwrong 9,s  nright 7,s  n 5,s  i 3,s  rbase 1,s
prim_partition:
        jsr     force_pop_t0
        jsr     need_list
        jsr     aux_t0
        sta     n0
        jsr     rpush_p0        ; list
        jsr     pop_p0          ; f
        jsr     rpush_p0
        lda     n0
        jsr     list_alloc
        jsr     rpush_obj       ; right
        lda     n0
        jsr     list_alloc
        jsr     rpush_obj       ; wrong
        lda     #0
        pha                     ; nwrong
        pha                     ; nright
        lda     n0
        pha
        lda     #0
        pha                     ; i
        lda     rsp
        sec
        sbc     #16
        pha
@loop:  lda     3,s
        cmp     5,s
        beq     @done
        jsr     robj_0
        lda     3,s
        jsr     elem_t0
        lda     p0
        sta     p1
        lda     p0+2
        sta     p1+2
        lda     1,s
        clc
        adc     #4
        tax
        jsr     rget_x          ; f
        jsr     call_with
        jsr     force_pop_t0
        jsr     need_bool
        ldy     #4
        lda     (t0),y
        and     #$00FF
        sta     t2              ; no bytecode until it is used
        jsr     robj_0
        lda     3,s
        jsr     elem_t0         ; p0 = the element
        lda     t2
        beq     @wrong
        jsr     robj_2_keep     ; right, keeping p0
        lda     7,s
        jsr     set_elem_obj
        lda     7,s
        inc     a
        sta     7,s
        bra     @next
@wrong: jsr     robj_3_keep     ; wrong, keeping p0
        lda     9,s
        jsr     set_elem_obj
        lda     9,s
        inc     a
        sta     9,s
@next:  lda     3,s
        inc     a
        sta     3,s
        bra     @loop
@done:  jsr     robj_2
        lda     t0
        sta     obj
        lda     7,s
        sta     t2
        lda     #T_LIST
        jsr     hdr_obj
        jsr     robj_3
        lda     t0
        sta     obj
        lda     9,s
        sta     t2
        lda     #T_LIST
        jsr     hdr_obj
        lda     #2
        jsr     attrs_alloc     ; { right; wrong; }
        lda     #2
        sta     n0
        lda     #str_right
        sta     t0
        jsr     hash_t0
        lda     #str_right
        sta     p1
        lda     #$8000
        sta     p1+2
        lda     1,s
        clc
        adc     #8
        tax
        lda     RSTACK,x
        sta     p2
        lda     RSTACK+2,x
        sta     p2+2            ; right
        stz     ix
        jsr     attrs_set_entry
        lda     #str_wrong
        sta     t0
        jsr     hash_t0
        lda     #str_wrong
        sta     p1
        lda     1,s
        clc
        adc     #12
        tax
        lda     RSTACK,x
        sta     p2
        lda     RSTACK+2,x
        sta     p2+2            ; wrong
        lda     #1
        sta     ix
        jsr     attrs_set_entry
        pla
        sta     rsp
        tsc
        clc
        adc     #8
        tcs
        jmp     push_obj_run

; robj_k_keep: like robj_k but leaves p0 alone (obj <- the object's address).
robj_2_keep:
        lda     3,s
        clc
        adc     #8
        tax
        bra     robj_keep_x
robj_3_keep:
        lda     3,s
        clc
        adc     #12
        tax
robj_keep_x:
        lda     RSTACK,x        ; young: the low word is the address
        sta     obj
        rts

; --- strings ---------------------------------------------------------------------------
prim_stringlength:
        jsr     force_pop_t0
        jsr     need_string
        jsr     aux_t0
        jsr     push_int16
        jmp     run

; substring start len s
prim_substring:
        jsr     force_pop_t0
        jsr     need_string
        jsr     rpush_p0        ; s, a root while len and start are forced
        jsr     force_pop_t0    ; len: negative means "to the end"
        jsr     need_int
        ldy     #10
        lda     (t0),y
        bmi     @to_end
        ldy     #6
        lda     (t0),y
        bne     @to_end         ; huge: also to the end
        ldy     #4
        lda     (t0),y
        bra     @have_len
@to_end:
        lda     #$FFFF
@have_len:
        pha                     ; len
        jsr     force_pop_t0    ; start
        jsr     need_int
        ldy     #10
        lda     (t0),y
        bmi     index_err2
        ldy     #6
        lda     (t0),y
        bne     @past          ; start beyond any string
        ldy     #4
        lda     (t0),y
        sta     n0              ; start
        pla
        sta     n1              ; len
        jsr     rpop_p0
        jsr     deref           ; t0 = s
        jsr     aux_t0
        sta     slen            ; |s|
        lda     n0
        cmp     slen
        bcs     @empty
        ; end = min(|s|, start + len) (len $FFFF means the whole rest)
        lda     n1
        cmp     #$FFFF
        beq     @rest
        clc
        adc     n0
        bcs     @rest
        cmp     slen
        bcs     @rest
        sta     kx              ; end
        bra     @cut
@rest:  lda     slen
        sta     kx
@cut:   lda     kx
        sec
        sbc     n0
        sta     n1              ; bytes
        lda     t0
        pha
        lda     n1
        jsr     str_alloc
        pla
        clc
        adc     #4
        clc
        adc     n0
        tax
        lda     obj
        clc
        adc     #4
        tay
        lda     n1
        jsr     copy_bytes
        jsr     push_p0
        jmp     run
@past:  pla
        jsr     rpop_p0
@empty: lda     #0
        jsr     str_alloc
        jsr     push_p0
        jmp     run
index_err2:
        jmp     index_err

; toString. A list is its elements' strings joined by spaces (Nix's coercion),
; so a throwing element propagates -- catchably -- through it.
prim_tostring:
        jsr     tostring_top
        jmp     run

; tostring_top: the value on top of the stack is replaced by its STRING form.
; Runs bytecode (forces; recurses into lists).
tostring_top:
        jsr     force_pop_t0
        lda     (t0)
        and     #$00FF
        cmp     #T_STRING
        beq     @same
        cmp     #T_INT
        jeq     @int
        cmp     #T_BOOL
        jeq     @bool
        cmp     #T_NULL
        jeq     @empty
        cmp     #T_LIST
        beq     @list
        jmp     type_err
@same:  jmp     push_p0
@list:  ; map tostring over the elements into a fresh list, then join with " "
        ; Roots: source, mapped. CPU: i 3,s  rbase 1,s
        jsr     aux_t0
        sta     n0
        jsr     rpush_p0        ; source
        lda     n0
        jsr     list_alloc      ; the converted elements
        jsr     rpush_obj       ; mapped
        lda     #0
        pha
        lda     rsp
        sec
        sbc     #8
        pha
@ll:    jsr     robj_0
        jsr     aux_t0
        cmp     3,s
        beq     @joined
        lda     3,s
        jsr     elem_t0
        jsr     push_p0
        jsr     tostring_top    ; recursion, frame-based
        jsr     pop_p0
        lda     p0
        sta     p1
        lda     p0+2
        sta     p1+2
        jsr     robj_1
        lda     t0
        sta     obj
        lda     p1
        sta     p0
        lda     p1+2
        sta     p0+2
        lda     3,s
        jsr     set_elem_obj
        lda     3,s
        inc     a
        sta     3,s
        bra     @ll
@joined:
        jsr     robj_1
        pla
        sta     rsp
        pla
        lda     p0
        sta     p1
        lda     p0+2
        sta     p1+2
        lda     #str_space
        jsr     push_static     ; sep
        lda     p1
        sta     p0
        lda     p1+2
        sta     p0+2
        jsr     push_p0         ; the mapped list
        jmp     concat_strings_sep_top
@bool:  ldy     #4
        lda     (t0),y
        and     #$00FF
        beq     @empty
        lda     #str_one
        jmp     push_static
@empty: lda     #0
        jsr     str_alloc
        jmp     push_p0
@int:   jsr     int_to_str
        jmp     push_p0

; push_obj_run_nojmp: push the object at obj (young) without dispatching.
push_obj_run_nojmp:
        lda     obj
        sta     p0
        lda     #$8000
        sta     p0+2
        jmp     push_p0
@bool:  ldy     #4
        lda     (t0),y
        and     #$00FF
        beq     @empty
        lda     #str_one
        jsr     push_static
        jmp     run
@empty: lda     #0
        jsr     str_alloc
        jsr     push_p0
        jmp     run
@int:   jsr     int_to_str
        jsr     push_p0
        jmp     run

; int_to_str: the INT at t0 -> obj/p0 a decimal STRING. Signed 64-bit, via
; repeated subtraction of powers of ten (no divide on this CPU; the card's
; would cost a bus round trip per digit).
int_to_str:
        ldy     #4
        ldx     #0
@cp:    lda     (t0),y
        sta     NUMBUF,x
        iny
        iny
        inx
        inx
        cpx     #8
        bne     @cp
        stz     kx              ; digits emitted
        lda     NUMBUF+6
        bpl     @pos
        ; negate: two's complement of the 8 bytes (Y counts: CPX would
        ; clobber the borrow)
        sec
        ldx     #0
        ldy     #4
@neg:   lda     #0
        sbc     NUMBUF,x
        sta     NUMBUF,x
        inx
        inx
        dey
        bne     @neg
        lda     #'-'
        sta     DIGITS
        inc     kx
@pos:   stz     ix              ; power index 0..19 (10^19 first)
@power: lda     ix
        cmp     #20
        jeq     @finish
        asl
        asl
        asl
        tax                     ; 8 bytes per power
        stz     jx              ; this digit
@sub:   ; NUMBUF >= pow10[x] ?
        lda     NUMBUF+6
        cmp     pow10+6,x
        bcc     @digit
        bne     @take
        lda     NUMBUF+4
        cmp     pow10+4,x
        bcc     @digit
        bne     @take
        lda     NUMBUF+2
        cmp     pow10+2,x
        bcc     @digit
        bne     @take
        lda     NUMBUF
        cmp     pow10,x
        bcc     @digit
@take:  sec
        lda     NUMBUF
        sbc     pow10,x
        sta     NUMBUF
        lda     NUMBUF+2
        sbc     pow10+2,x
        sta     NUMBUF+2
        lda     NUMBUF+4
        sbc     pow10+4,x
        sta     NUMBUF+4
        lda     NUMBUF+6
        sbc     pow10+6,x
        sta     NUMBUF+6
        inc     jx
        bra     @sub
@digit: lda     jx
        bne     @emit
        lda     kx              ; leading zero: skip unless something was emitted
        beq     @skip
        lda     DIGITS
        and     #$00FF
        cmp     #'-'
        bne     @emit
        lda     kx
        cmp     #1
        beq     @skip           ; only the sign so far
@emit:  lda     jx
        clc
        adc     #'0'
        sep     #$20
        .a8
        ldy     kx
        sta     DIGITS,y
        rep     #$20
        .a16
        inc     kx
@skip:  inc     ix
        jmp     @power
@finish:
        lda     kx
        bne     @nonzero
        lda     #'0'
        sep     #$20
        .a8
        sta     DIGITS
        rep     #$20
        .a16
        inc     kx
@nonzero:
        lda     kx
        cmp     #1
        bne     @alloc
        lda     DIGITS
        and     #$00FF
        cmp     #'-'
        bne     @alloc
        lda     #'0'            ; "-" alone cannot happen, but be safe
        sep     #$20
        .a8
        sta     DIGITS
        rep     #$20
        .a16
@alloc: lda     kx
        jsr     str_alloc
        ldx     #DIGITS
        lda     obj
        clc
        adc     #4
        tay
        lda     kx
        jmp     copy_bytes

        ; 10^19 .. 10^0 as (low dword, high dword)
pow10:  .dword  $89E80000, $8AC72304    ; 10^19
        .dword  $A7640000, $0DE0B6B3    ; 10^18
        .dword  $5D8A0000, $01634578    ; 10^17
        .dword  $6FC10000, $002386F2    ; 10^16
        .dword  $A4C68000, $00038D7E    ; 10^15
        .dword  $107A4000, $00005AF3    ; 10^14
        .dword  $4E72A000, $00000918    ; 10^13
        .dword  $D4A51000, $000000E8    ; 10^12
        .dword  $4876E800, $00000017    ; 10^11
        .dword  $540BE400, $00000002    ; 10^10
        .dword  $3B9ACA00, $00000000    ; 10^9
        .dword  $05F5E100, $00000000    ; 10^8
        .dword  $00989680, $00000000    ; 10^7
        .dword  $000F4240, $00000000    ; 10^6
        .dword  $000186A0, $00000000    ; 10^5
        .dword  $00002710, $00000000    ; 10^4
        .dword  $000003E8, $00000000    ; 10^3
        .dword  $00000064, $00000000    ; 10^2
        .dword  $0000000A, $00000000    ; 10^1
        .dword  $00000001, $00000000    ; 10^0

; concatStringsSep sep list. Frame: written 1,s  i 3,s  n 5,s  list 7,s  sep 9,s
prim_concatstringssep:          ; sep list, list on top
        jsr     concat_strings_sep_top
        jmp     run

; concat_strings_sep_top: as above, but returns with the result on the stack
; (toString's list case shares it).
; Roots: sep, list. CPU: total 7,s  n 5,s  i 3,s  rbase 1,s
concat_strings_sep_top:
        jsr     force_two       ; t0 = sep, t1 = list
        jsr     need_string
        lda     (t1)
        and     #$00FF
        cmp     #T_LIST
        jne     type_err
        jsr     aux_t1
        sta     n0
        lda     t0
        sta     p0
        lda     #$8000
        sta     p0+2
        jsr     rpush_p0        ; sep
        lda     t1
        sta     p0
        jsr     rpush_p0        ; list
        lda     #0
        pha                     ; total
        lda     n0
        pha
        lda     #0
        pha                     ; i
        lda     rsp
        sec
        sbc     #8
        pha
@count: lda     3,s
        cmp     5,s
        beq     @alloc
        jsr     robj_1
        lda     3,s
        jsr     elem_t0
        jsr     push_p0
        jsr     force_top
        jsr     pop_p0
        jsr     deref
        jsr     need_string
        jsr     aux_t0
        clc
        adc     7,s
        jcs     nomem
        sta     7,s
        lda     3,s
        inc     a
        sta     3,s
        bra     @count
@alloc: jsr     robj_0          ; sep (no bytecode from here on)
        jsr     aux_t0
        sta     n1              ; |sep|
        lda     5,s
        beq     @nosep
        dec     a
        sta     t4              ; n-1 separators: total += (n-1)*|sep|
@mul:   lda     t4
        beq     @nosep
        dec     t4
        lda     7,s
        clc
        adc     n1
        jcs     nomem
        sta     7,s
        bra     @mul
@nosep: lda     7,s
        jsr     str_alloc
        lda     #0
        sta     7,s             ; bytes written
        sta     3,s             ; i
@copy:  lda     3,s
        cmp     5,s
        beq     @done
        lda     3,s
        beq     @elem
        ; separator first
        jsr     robj_0
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
        adc     7,s
        tay
        lda     n1
        jsr     copy_bytes
        lda     7,s
        clc
        adc     n1
        sta     7,s
@elem:  jsr     robj_1
        lda     3,s
        jsr     elem_t0
        jsr     deref_follow
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
        adc     7,s
        tay
        lda     n1
        jsr     copy_bytes
        lda     7,s
        clc
        adc     n1
        sta     7,s
        lda     3,s
        inc     a
        sta     3,s
        bra     @copy
@done:  pla
        sta     rsp
        tsc
        clc
        adc     #6
        tcs
        jmp     push_obj_run_nojmp  ; (elem_t0 above clobbered p0); returns

; baseNameOf / dirOf on STRINGs, with Nix's edge cases.
prim_basenameof:
        jsr     force_pop_t0
        jsr     need_string
        jsr     aux_t0
        sta     slen
        beq     @empty
        dec     a
        sta     kx              ; last index
        ; a trailing slash is ignored (unless it is the only character)
        lda     kx
        beq     @scan
        ldy     kx
        jsr     byte_at_t0
        cmp     #'/'
        bne     @scan
        dec     kx
@scan:  lda     kx
        sta     jx              ; scan from `last` down for a slash
@loop:  ldy     jx
        jsr     byte_at_t0
        cmp     #'/'
        beq     @found
        lda     jx
        beq     @none
        dec     jx
        bra     @loop
@found: inc     jx              ; start = pos + 1
        bra     @cut
@none:  stz     jx
@cut:   lda     kx
        sec
        sbc     jx
        inc     a               ; length = last - start + 1
        sta     n1
        lda     t0
        pha
        lda     n1
        jsr     str_alloc
        pla
        clc
        adc     #4
        clc
        adc     jx
        tax
        lda     obj
        clc
        adc     #4
        tay
        lda     n1
        jsr     copy_bytes
        jsr     push_p0
        jmp     run
@empty: lda     #0
        jsr     str_alloc
        jsr     push_p0
        jmp     run

; byte_at_t0: Y = index -> A = byte Y of the STRING at t0
byte_at_t0:
        iny
        iny
        iny
        iny
        lda     (t0),y
        and     #$00FF
        rts

prim_dirof:
        jsr     force_pop_t0
        jsr     need_string
        jsr     aux_t0
        sta     slen
        beq     @dot
        dec     a
        sta     jx
@loop:  ldy     jx
        jsr     byte_at_t0
        cmp     #'/'
        beq     @found
        lda     jx
        beq     @dot
        dec     jx
        bra     @loop
@found: lda     jx
        bne     @cut
        lda     #str_slash
        jsr     push_static
        jmp     run
@cut:   lda     t0
        pha
        lda     jx
        jsr     str_alloc
        pla
        clc
        adc     #4
        tax
        lda     obj
        clc
        adc     #4
        tay
        lda     jx
        jsr     copy_bytes
        jsr     push_p0
        jmp     run
@dot:   lda     #str_dot
        jsr     push_static
        jmp     run

; --- tryEval: catch frames (BYTECODE.md) --------------------------------------------
; The argument is on top, unforced. Save a frame, force it; the error exit
; unwinds here (catch_resume) on throw/assert with everything restored.
prim_tryeval:
        lda     catch_n
        cmp     #CATCH_MAX
        jcs     nomem
        asl
        asl
        asl
        asl                     ; * 16
        tax
        tsc
        sta     CATCH+0,x
        lda     vsp
        sta     CATCH+2,x
        lda     env
        sta     CATCH+4,x
        lda     env+2
        sta     CATCH+6,x
        lda     pc
        sta     CATCH+8,x
        lda     ctab
        sta     CATCH+10,x
        lda     fstack_n
        sta     CATCH+12,x
        lda     rsp
        sta     CATCH+14,x
        inc     catch_n
        jsr     force_top
        dec     catch_n
        jsr     pop_p0          ; the value
        lda     p0
        sta     p2
        lda     p0+2
        sta     p2+2
        lda     #obj_true
        sta     sav
        bra     tryeval_build
catch_resume:
        jsr     pop_p0          ; the argument's slot
        lda     #obj_false
        sta     sav
        sta     p2
        lda     #$8000
        sta     p2+2
; { success = <sav>; value = <p2>; }
tryeval_build:
        lda     p2
        pha
        lda     p2+2
        pha
        lda     #2
        jsr     attrs_alloc
        lda     #2
        sta     n0
        lda     #str_success
        sta     t0
        jsr     hash_t0
        lda     #str_success
        sta     p1
        lda     #$8000
        sta     p1+2
        lda     sav
        sta     p2
        lda     #$8000
        sta     p2+2
        stz     ix
        jsr     attrs_set_entry
        lda     #str_value
        sta     t0
        jsr     hash_t0
        lda     #str_value
        sta     p1
        pla
        sta     p2+2
        pla
        sta     p2
        lda     #1
        sta     ix
        jsr     attrs_set_entry
        jmp     push_obj_run

; --- errors and forcing --------------------------------------------------------------
prim_throw:
        lda     #E_THROW
        bra     throw_exit
prim_abort:
        lda     #E_ABORT
throw_exit:
        pha
        jsr     force_pop_t0
        jsr     need_string
        lda     p0
        sta     RESULT
        lda     p0+2
        sta     RESULT+2
        pla
        jmp     err_exit

prim_seq:
        jsr     pop_p0          ; b, a root while a is forced
        jsr     rpush_p0
        jsr     force_top
        jsr     pop_p0
        jsr     rpop_p0
        jsr     push_p0
        jmp     run

prim_deepseq:
        jsr     pop_p0          ; b
        jsr     rpush_p0
        jsr     deep_force_top
        jsr     pop_p0
        jsr     rpop_p0
        jsr     push_p0
        jmp     run

; --- types ------------------------------------------------------------------------------
prim_typeof:
        jsr     force_pop_t0
        lda     (t0)
        and     #$00FF
        asl
        tax
        lda     typenames,x
        beq     type_err2
        jsr     push_static
        jmp     run
type_err2:
        jmp     type_err

typenames:
        .word   0, str_int, str_bool, str_null, str_string, 0, str_list, str_set
        .word   0, str_lambda, 0, 0, 0, 0, 0, 0

prim_isint:
        lda     #T_INT
        bra     is_tag
prim_isbool:
        lda     #T_BOOL
        bra     is_tag
prim_isstring:
        lda     #T_STRING
        bra     is_tag
prim_isnull:
        lda     #T_NULL
        bra     is_tag
prim_islist:
        lda     #T_LIST
        bra     is_tag
prim_isattrs:
        lda     #T_ATTRS
        bra     is_tag
prim_isfunction:
        lda     #T_LAMBDA
is_tag: pha
        jsr     force_pop_t0
        lda     (t0)
        and     #$00FF
        sta     t2
        pla
        cmp     t2
        jsr     push_bool_z
        jmp     run

; --- bit operations ----------------------------------------------------------------------
prim_bitand:
        jsr     binary_ints
        jsr     new_int
        ldy     #4
        ldx     #4
@w:     lda     (t0),y
        and     (t1),y
        sta     (obj),y
        iny
        iny
        dex
        bne     @w
        jsr     push_p0
        jmp     run
prim_bitor:
        jsr     binary_ints
        jsr     new_int
        ldy     #4
        ldx     #4
@w:     lda     (t0),y
        ora     (t1),y
        sta     (obj),y
        iny
        iny
        dex
        bne     @w
        jsr     push_p0
        jmp     run
prim_bitxor:
        jsr     binary_ints
        jsr     new_int
        ldy     #4
        ldx     #4
@w:     lda     (t0),y
        eor     (t1),y
        sta     (obj),y
        iny
        iny
        dex
        bne     @w
        jsr     push_p0
        jmp     run

; --- static strings ----------------------------------------------------------------------
; STRING objects the VM hands out: header (aux = length), bytes padded to a
; word, null context. Immutable.
.macro  static_str name, text
name:   .byte   T_STRING, .strlen(text), 0, 0
        .byte   text
        .res    (4 - (.strlen(text) & 3)) & 3, 0
        .dword  0
.endmacro

static_str str_int,     "int"
static_str str_bool,    "bool"
static_str str_null,    "null"
static_str str_string,  "string"
static_str str_list,    "list"
static_str str_set,     "set"
static_str str_lambda,  "lambda"
static_str str_one,     "1"
static_str str_dot,     "."
static_str str_slash,   "/"
static_str str_name,    "name"
static_str str_value,   "value"
static_str str_right,   "right"
static_str str_wrong,   "wrong"
static_str str_success, "success"
static_str str_functor, "__functor"
static_str str_space,   " "
