; ---------------------------------------------------------------------------
; vm.s -- the Nix bytecode VM, slice 1. 65816 native mode, 16-bit A/X/Y.
;
; Executes an .nbc image (docs/BYTECODE.md) already in memory at NBC_BASE and
; writes the entry chunk's value to RESULT. Slice 1: integers, booleans, null,
; let, if, the arithmetic and boolean operators, and lazy thunks with in-place
; update. Multiply and divide go to the card's ALU64 verb; everything else
; stays on the CPU, because the D5 gate says a 64-bit add or compare is ~40
; cycles here and ~320 through the slot.
;
; The design (BYTECODE.md "The VM's registers and stacks"):
;
;   pc    next bytecode byte            ctab  this chunk's constant pointers
;   env   current environment LIST      vsp   value stack pointer (32-bit slots)
;   nfree nursery bump pointer          obj   the object alloc just returned
;
; FORCE recurses through the CPU's own stack: it saves pc/ctab/env, runs the
; thunk's chunk with `jsr run`, and RETURN is simply `rts`. The native-mode stack
; pointer is 16 bits, so the stack is moved to CPU_STACK for the VM's lifetime
; and restored before returning to the (emulation-mode) caller.
;
; Memory map, bank 0:
;   $0300 RESULT     $0320 status   $0321 error code   (harness contract)
;   $0C00 CHUNK_INFO 4 bytes/chunk: code address, constant-table address
;   $1000 CTAB       constant pointer tables         $1800 CPU stack (grows down to $1840)
;   $2000 this program                                $4000 NURSERY (16 KB)
;   $8000 the .nbc image (8 KB max)                   $A000 value stack (8 KB)
;
; Every failure is LOUD: an unknown opcode, a bad type, a null slot, a full
; nursery or stack, a missing card -- each writes $FF and an E_* code, never a
; plausible value. Unknown opcodes in particular abort rather than skip.
; ---------------------------------------------------------------------------

        .setcpu "65816"
        .include "opcodes.inc"

; Long conditional branches. A 65816 branch reaches +-127 bytes, and the shared
; error exits sit well beyond that from most of their callers.
.macro  jne  target
        beq     :+
        jmp     target
:
.endmacro
.macro  jeq  target
        bne     :+
        jmp     target
:
.endmacro
.macro  jcs  target
        bcc     :+
        jmp     target
:
.endmacro
.macro  jcc  target
        bcs     :+
        jmp     target
:
.endmacro

RESULT          = $0300
RESULT_STAT     = $0320
RESULT_ERR      = $0321

CHUNK_INFO      = $0C00         ; 256 chunks max
CTAB            = $1000
CTAB_END        = $1800
CPU_STACK_MIN   = $1840         ; FORCE refuses to nest below this
CPU_STACK       = $1FFF
NURSERY         = $4000
NURSERY_END     = $8000
NBC_BASE        = $8000
NBC_MAX         = $2000
VSTACK          = $A000
VSTACK_END      = $C000

T_INT           = $01
T_BOOL          = $02
T_NULL          = $03
T_LIST          = $06
T_THUNK         = $08
T_INDIRECT      = $0A
T_BLACKHOLE     = $0B

REG_CMD         = $C080
REG_CTRL        = $C081
REG_PSEL        = $C086
REG_PPORT       = $C087
CARD_ID         = $A2
V_ALU64         = $50
ST_DONE         = $40
ST_ERR          = $20
A_MUL           = $02
A_DIV           = $03

        .zeropage
pc:             .res 2
ctab:           .res 2
env:            .res 2          ; 16-bit address of the current env LIST; 0 = none
vsp:            .res 2
nfree:          .res 2
obj:            .res 2          ; address of the object alloc just returned
p0:             .res 4          ; a 32-bit value
p1:             .res 4
t0:             .res 2          ; scratch, usually an object address
t1:             .res 2
t2:             .res 2
t3:             .res 2
q:              .res 2          ; loader cursor
cnt:            .res 2
ci:             .res 2          ; loader: chunk index * 4
cnext:          .res 2          ; loader: next free CTAB slot
nchunks:        .res 2
slot_x:         .res 2          ; card slot * $10, or 0 if no card
saved_sp:       .res 2

        .code

; ===========================================================================
start:
        clc
        xce                     ; native mode
        rep     #$30
        .a16
        .i16
        tsc
        sta     saved_sp
        lda     #CPU_STACK
        tcs

        ; find the card (ID $A2 at $C0n1), slot 7 down to 1
        sep     #$20
        .a8
        ldx     #$0070
@scan:  lda     REG_CTRL,x
        cmp     #CARD_ID
        beq     @found
        rep     #$20
        .a16
        txa
        sec
        sbc     #$10
        tax
        sep     #$20
        .a8
        cpx     #0
        bne     @scan
@found: rep     #$20
        .a16
        stx     slot_x

        lda     #NURSERY
        sta     nfree
        lda     #VSTACK
        sta     vsp
        stz     env

        jsr     load
        jsr     run             ; the entry chunk; RETURN brings us back here

        ; --- report the result ---
        jsr     force_top       ; a strict body already forced it; be sure
        jsr     pop_p0
        jsr     deref
        lda     (t0)
        and     #$00FF
        sta     t2
        sep     #$20
        .a8
        sta     RESULT
        rep     #$20
        .a16
        cmp     #T_INT
        bne     @notint
        ldy     #4
        lda     (t0),y
        sta     RESULT+1
        ldy     #6
        lda     (t0),y
        sta     RESULT+3
        ldy     #8
        lda     (t0),y
        sta     RESULT+5
        ldy     #10
        lda     (t0),y
        sta     RESULT+7
        bra     @ok
@notint:
        cmp     #T_BOOL
        bne     @notbool
        ldy     #4
        lda     (t0),y
        and     #$00FF
        sta     RESULT+1
        bra     @ok
@notbool:
        cmp     #T_NULL
        beq     @ok
        lda     #E_TYPE
        jmp     err_exit
@ok:    sep     #$20
        .a8
        lda     #1
        sta     RESULT_STAT
        rep     #$20
        .a16
done:
        lda     saved_sp
        tcs
        sec
        xce                     ; back to emulation mode for the caller
        .a8
        .i8
        rts

        .a16
        .i16
; A (low byte) = error code. Restores the caller's stack, so it can be reached
; from any nesting depth.
err_exit:
        sep     #$20
        .a8
        sta     RESULT_ERR
        lda     #$FF
        sta     RESULT_STAT
        rep     #$20
        .a16
        bra     done

; ===========================================================================
; Loader: validate the header, materialise every chunk's constants into the
; nursery, and record each chunk's code and constant-table addresses.
load:
        lda     NBC_BASE        ; "NB"
        cmp     #$424E
        jne     badnbc
        lda     NBC_BASE+2      ; "C", version 1
        cmp     #$0143
        jne     badnbc
        lda     NBC_BASE+10     ; total_size, high word
        jne     badnbc
        lda     NBC_BASE+8
        cmp     #NBC_MAX+1
        jcs     badnbc
        lda     NBC_BASE+4      ; chunk_count
        jeq     badnbc
        cmp     #257
        jcs     badnbc
        sta     nchunks
        lda     #CTAB
        sta     cnext
        stz     ci

@chunk: ldx     ci
        lda     NBC_BASE+18,x   ; offset high word must be 0
        jne     badnbc
        lda     NBC_BASE+16,x
        clc
        adc     #NBC_BASE
        sta     t1              ; chunk header
        lda     (t1)
        sta     cnt             ; const_count
        lda     t1
        clc
        adc     #4
        sta     q               ; first constant
        lda     cnext
        sta     CHUNK_INFO+2,x  ; this chunk's constant table

@const: lda     cnt
        beq     @code
        dec     cnt
        lda     (q)
        and     #$00FF
        cmp     #K_INT
        jne     badnbc
        lda     #12
        jsr     alloc           ; obj, p0
        lda     #T_INT
        sta     (obj)
        lda     #0
        ldy     #2
        sta     (obj),y
        ldy     #1              ; 8 payload bytes, unaligned source is fine
        lda     (q),y
        ldy     #4
        sta     (obj),y
        ldy     #3
        lda     (q),y
        ldy     #6
        sta     (obj),y
        ldy     #5
        lda     (q),y
        ldy     #8
        sta     (obj),y
        ldy     #7
        lda     (q),y
        ldy     #10
        sta     (obj),y
        lda     cnext
        cmp     #CTAB_END
        bcs     nomem
        lda     p0
        sta     (cnext)
        ldy     #2
        lda     p0+2
        sta     (cnext),y
        lda     cnext
        clc
        adc     #4
        sta     cnext
        lda     q
        clc
        adc     #9
        sta     q
        bra     @const

@code:  ldx     ci
        lda     q               ; code follows the constants
        sta     CHUNK_INFO,x
        lda     ci
        clc
        adc     #4
        sta     ci
        lsr
        lsr
        cmp     nchunks
        jcc     @chunk

        lda     NBC_BASE+6      ; entry chunk
        cmp     nchunks
        jcs     badnbc
        asl
        asl
        tax
        lda     CHUNK_INFO,x
        sta     pc
        lda     CHUNK_INFO+2,x
        sta     ctab
        rts

badnbc: lda     #E_BADNBC
        jmp     err_exit
nomem:  lda     #E_NOMEM
        jmp     err_exit

; ===========================================================================
; The nursery: bump allocation only, in slice 1. A = size (multiple of 4, >= 8).
; Returns obj = address, p0 = the young pointer (bit 31 set). Preserves X.
alloc:
        sta     t3
        lda     nfree
        sta     obj
        clc
        adc     t3
        bcs     nomem
        cmp     #NURSERY_END+1
        bcs     nomem
        sta     nfree
        lda     obj
        sta     p0
        lda     #$8000
        sta     p0+2
        rts

; ===========================================================================
; Value stack helpers. Slots are 32-bit; p0 is the transfer register.
push_p0:
        lda     vsp
        cmp     #VSTACK_END
        bcs     nomem
        lda     p0
        sta     (vsp)
        ldy     #2
        lda     p0+2
        sta     (vsp),y
        lda     vsp
        clc
        adc     #4
        sta     vsp
        rts

pop_p0:
        lda     vsp
        sec
        sbc     #4
        sta     vsp
        lda     (vsp)
        sta     p0
        ldy     #2
        lda     (vsp),y
        sta     p0+2
        rts

peek_p0:
        lda     vsp
        sec
        sbc     #4
        sta     t3
        lda     (t3)
        sta     p0
        ldy     #2
        lda     (t3),y
        sta     p0+2
        rts

poke_p0:                        ; replace the top with p0
        lda     vsp
        sec
        sbc     #4
        sta     t3
        lda     p0
        sta     (t3)
        ldy     #2
        lda     p0+2
        sta     (t3),y
        rts

; p0 -> t0 (a bank-0 address). Slice 1 handles young pointers only; a mature
; pointer or a null is an error, never a guess.
deref:
        lda     p0+2
        bpl     @bad
        lda     p0
        beq     @bad
        sta     t0
        rts
@bad:   lda     #E_BADNBC
        jmp     err_exit

; Push one of the shared singletons: A = its address.
push_static:
        sta     p0
        lda     #$8000
        sta     p0+2
        jmp     push_p0

; ===========================================================================
; Operand fetch.
fetch8:
        lda     (pc)
        and     #$00FF
        inc     pc
        rts

fetch16:
        lda     (pc)
        inc     pc
        inc     pc
        rts

; ===========================================================================
; The dispatch loop. Each handler ends in `jmp run`; RETURN is `rts`, which
; returns to whoever called run (FORCE, or start for the entry chunk).
run:
        jsr     fetch8
        cmp     #OP_RETURN+1
        jcs     op_bad
        asl
        tax
        jmp     (optable,x)

optable:
        .word   op_bad, op_const, op_pop, op_true, op_false, op_null     ; $00-$05
        .word   op_bad, op_bad, op_bad, op_bad, op_bad                   ; $06-$0A
        .word   op_bad, op_bad, op_bad, op_bad, op_bad                   ; $0B-$0F
        .word   op_add, op_sub, op_mul, op_div, op_neg, op_bad           ; $10-$15
        .word   op_eq, op_lt, op_le, op_gt, op_ge, op_not                ; $16-$1B
        .word   op_bad, op_bad, op_bad, op_bad                           ; $1C-$1F
        .word   op_jmp, op_jmp_if_false, op_jmp_if_true                  ; $20-$22
        .word   op_bad, op_bad, op_bad, op_bad, op_bad, op_bad, op_bad   ; $23-$29
        .word   op_bad, op_bad, op_bad, op_bad, op_bad, op_bad           ; $2A-$2F
        .word   op_env_new, op_env_set, op_env_get, op_env_pop           ; $30-$33
        .word   op_bad, op_bad, op_bad, op_bad, op_bad, op_bad           ; $34-$39
        .word   op_bad, op_bad, op_bad, op_bad, op_bad, op_bad           ; $3A-$3F
        .word   op_thunk, op_force, op_return                            ; $40-$42

op_bad: lda     #E_BADOP
        jmp     err_exit

; --- constants and singletons ---------------------------------------------
op_const:
        jsr     fetch16
        asl
        asl
        tay
        lda     (ctab),y
        sta     p0
        iny
        iny
        lda     (ctab),y
        sta     p0+2
        jsr     push_p0
        jmp     run

op_pop: lda     vsp
        sec
        sbc     #4
        sta     vsp
        jmp     run

op_true:
        lda     #obj_true
        jsr     push_static
        jmp     run
op_false:
        lda     #obj_false
        jsr     push_static
        jmp     run
op_null:
        lda     #obj_null
        jsr     push_static
        jmp     run

; --- integer arithmetic on the CPU ------------------------------------------
; Forces and pops both operands: t0 = a, t1 = b, both verified INT.
binary_ints:
        jsr     force_top
        jsr     pop_p0
        jsr     deref
        jsr     need_int
        lda     t0
        sta     t1
        jsr     force_top
        jsr     pop_p0
        jsr     deref
        jmp     need_int

need_int:                       ; t0 must be an INT
        lda     (t0)
        and     #$00FF
        cmp     #T_INT
        jne     type_err
        rts
type_err:
        lda     #E_TYPE
        jmp     err_exit

; A fresh INT in obj/p0, header written; the caller fills the payload.
new_int:
        lda     #12
        jsr     alloc
        lda     #T_INT
        sta     (obj)
        lda     #0
        ldy     #2
        sta     (obj),y
        rts

; The word loops below count with DEX, never CPY: a compare rewrites the carry
; the next ADC/SBC depends on. The first draft used CPY and every value with a
; non-zero upper word came out off by 65536 -- the same trap mul64_bench.s
; documents, caught here by tests/fixtures/eval-okay-carry-chain.nix.
op_add:
        jsr     binary_ints
        jsr     new_int
        clc
        ldy     #4
        ldx     #4
@w:     lda     (t0),y
        adc     (t1),y
        sta     (obj),y
        iny
        iny
        dex
        bne     @w
        jsr     push_p0
        jmp     run

op_sub:
        jsr     binary_ints
        jsr     new_int
        sec
        ldy     #4
        ldx     #4
@w:     lda     (t0),y
        sbc     (t1),y
        sta     (obj),y
        iny
        iny
        dex
        bne     @w
        jsr     push_p0
        jmp     run

op_neg:
        jsr     force_top
        jsr     pop_p0
        jsr     deref
        jsr     need_int
        jsr     new_int
        sec
        ldy     #4
        ldx     #4
@w:     lda     #0
        sbc     (t0),y
        sta     (obj),y
        iny
        iny
        dex
        bne     @w
        jsr     push_p0
        jmp     run

; --- multiply and divide on the card ----------------------------------------
; ALU64 (CARD-ISA.md): P0 <- a, P1 <- b, P2 <- op, CMD <- ALU64, poll, P3 -> result.
; The registers are 8-bit ports, so this is done with an 8-bit accumulator: a
; 16-bit store to $C0n7 would write $C0n8 as well.
op_mul:
        lda     #A_MUL
        bra     alu
op_div:
        lda     #A_DIV
alu:    sta     t2
        jsr     binary_ints
        ldx     slot_x
        beq     nocard
        sep     #$20
        .a8
        lda     #0
        sta     REG_PSEL,x
        ldy     #4
@wa:    lda     (t0),y
        sta     REG_PPORT,x
        iny
        cpy     #12
        bne     @wa
        lda     #1
        sta     REG_PSEL,x
        ldy     #4
@wb:    lda     (t1),y
        sta     REG_PPORT,x
        iny
        cpy     #12
        bne     @wb
        lda     #2
        sta     REG_PSEL,x
        lda     t2
        sta     REG_PPORT,x
        lda     #V_ALU64
        sta     REG_CMD,x
@wait:  lda     REG_CMD,x
        bit     #(ST_DONE|ST_ERR)
        beq     @wait
        and     #ST_ERR
        bne     @err
        rep     #$20
        .a16
        jsr     new_int
        sep     #$20
        .a8
        lda     #3
        sta     REG_PSEL,x
        ldy     #4
@rd:    lda     REG_PPORT,x
        sta     (obj),y
        iny
        cpy     #12
        bne     @rd
        rep     #$20
        .a16
        jsr     push_p0
        jmp     run
        .a8
@err:   rep     #$20
        .a16
        lda     #E_DIVZERO      ; the only error ALU64 MUL/DIV can raise
        jmp     err_exit
nocard: lda     #E_NOCARD
        jmp     err_exit

; --- comparison ---------------------------------------------------------------
; cmp64: t0 = a, t1 = b (INTs). Returns A = 0 (a < b), 1 (equal), 2 (a > b).
; Signed 64-bit: the top word decides with N xor V after SBC, the lower words
; are compared unsigned. No 64-bit subtraction, so no overflow trap.
cmp64:
        ldy     #10
        lda     (t0),y
        cmp     (t1),y
        beq     @w8
        sec
        lda     (t0),y
        sbc     (t1),y
        bvc     @nov
        eor     #$8000
@nov:   bmi     @less
        bra     @greater
@w8:    ldy     #8
        lda     (t0),y
        cmp     (t1),y
        bcc     @less
        bne     @greater
        ldy     #6
        lda     (t0),y
        cmp     (t1),y
        bcc     @less
        bne     @greater
        ldy     #4
        lda     (t0),y
        cmp     (t1),y
        bcc     @less
        bne     @greater
        lda     #1
        rts
@less:  lda     #0
        rts
@greater:
        lda     #2
        rts

; Push TRUE if Z is set, FALSE otherwise.
push_bool_z:
        beq     @t
        lda     #obj_false
        jmp     push_static
@t:     lda     #obj_true
        jmp     push_static

op_lt:  jsr     binary_ints
        jsr     cmp64
        cmp     #0
        jsr     push_bool_z
        jmp     run
op_gt:  jsr     binary_ints
        jsr     cmp64
        cmp     #2
        jsr     push_bool_z
        jmp     run
op_le:  jsr     binary_ints
        jsr     cmp64
        cmp     #2
        beq     @f
        lda     #0
@f:     jsr     push_bool_z     ; Z set unless a > b
        jmp     run
op_ge:  jsr     binary_ints
        jsr     cmp64
        cmp     #0
        beq     @f
        lda     #0
        bra     @p
@f:     lda     #1
@p:     cmp     #0
        jsr     push_bool_z     ; Z set unless a < b
        jmp     run

; --- equality ------------------------------------------------------------------
op_eq:
        jsr     force_top
        jsr     pop_p0
        jsr     deref
        lda     t0
        sta     t1
        jsr     force_top
        jsr     pop_p0
        jsr     deref
        lda     (t0)
        and     #$00FF
        sta     t2
        lda     (t1)
        and     #$00FF
        cmp     t2
        bne     @false          ; different types are unequal
        cmp     #T_INT
        beq     @int
        cmp     #T_BOOL
        beq     @bool
        cmp     #T_NULL
        beq     @true
        jmp     type_err
@int:   ldy     #4
@w:     lda     (t0),y
        cmp     (t1),y
        bne     @false
        iny
        iny
        cpy     #12
        bne     @w
        bra     @true
@bool:  ldy     #4
        lda     (t0),y
        and     #$00FF
        sta     t2
        lda     (t1),y
        and     #$00FF
        cmp     t2
        bne     @false
@true:  lda     #obj_true
        jsr     push_static
        jmp     run
@false: lda     #obj_false
        jsr     push_static
        jmp     run

op_not:
        jsr     force_top
        jsr     pop_p0
        jsr     deref
        jsr     need_bool
        ldy     #4
        lda     (t0),y
        and     #$00FF
        jsr     push_bool_z     ; !false = true
        jmp     run

need_bool:
        lda     (t0)
        and     #$00FF
        cmp     #T_BOOL
        jne     type_err
        rts

; --- control flow ------------------------------------------------------------
op_jmp:
        jsr     fetch16
        clc
        adc     pc
        sta     pc
        jmp     run

; Both peek: the value stays on the stack for the compiler's POP.
op_jmp_if_false:
        jsr     fetch16
        sta     t2
        jsr     force_top
        jsr     peek_p0
        jsr     deref
        jsr     need_bool
        ldy     #4
        lda     (t0),y
        and     #$00FF
        bne     @no
        lda     pc
        clc
        adc     t2
        sta     pc
@no:    jmp     run

op_jmp_if_true:
        jsr     fetch16
        sta     t2
        jsr     force_top
        jsr     peek_p0
        jsr     deref
        jsr     need_bool
        ldy     #4
        lda     (t0),y
        and     #$00FF
        beq     @no
        lda     pc
        clc
        adc     t2
        sta     pc
@no:    jmp     run

; --- environments --------------------------------------------------------------
; ENV_NEW n: a LIST of n+1 pointers; [0] = the old env, [1..n] = null.
op_env_new:
        jsr     fetch16
        sta     t2              ; n
        inc     a
        sta     t1              ; count = n+1
        asl
        asl
        clc
        adc     #4
        jsr     alloc
        lda     t1              ; header: tag LIST, aux = count (24-bit LE)
        xba
        and     #$FF00
        ora     #T_LIST
        sta     (obj)
        lda     t1
        xba
        and     #$00FF
        ldy     #2
        sta     (obj),y
        ldy     #4              ; [0] = old env as a young pointer, or 0
        lda     env
        sta     (obj),y
        beq     @z
        lda     #$8000
@z:     iny
        iny
        sta     (obj),y
        ldx     t2
        beq     @done
        ldy     #8
        lda     #0
@clr:   sta     (obj),y
        iny
        iny
        sta     (obj),y
        iny
        iny
        dex
        bne     @clr
@done:  lda     obj
        sta     env
        jmp     run

op_env_set:
        jsr     fetch16
        asl
        asl
        clc
        adc     #4
        sta     t1              ; byte offset of element i (pop_p0 uses Y)
        jsr     pop_p0
        lda     env
        sta     t0
        ldy     t1
        lda     p0
        sta     (t0),y
        iny
        iny
        lda     p0+2
        sta     (t0),y
        jmp     run

op_env_get:
        jsr     fetch16
        sta     t2              ; depth
        jsr     fetch16
        asl
        asl
        clc
        adc     #4
        sta     t1              ; byte offset of element i
        lda     env
        sta     t0
@walk:  lda     t2
        beq     @got
        dec     t2
        ldy     #4
        lda     (t0),y          ; parent's low word is its address
        beq     badenv
        sta     t0
        bra     @walk
@got:   ldy     t1
        lda     (t0),y
        sta     p0
        iny
        iny
        lda     (t0),y
        sta     p0+2
        ora     p0
        beq     badenv          ; a slot read before it was set: generator bug
        jsr     push_p0
        jmp     run
badenv: lda     #E_BADNBC
        jmp     err_exit

op_env_pop:
        lda     env
        sta     t0
        ldy     #4
        lda     (t0),y
        sta     env
        jmp     run

; --- thunks -----------------------------------------------------------------------
; THUNK c: 12 bytes -- header, code address + constant table (the "code
; pointer"), and the current environment (the "env pointer").
op_thunk:
        jsr     fetch16
        asl
        asl
        tax                     ; CHUNK_INFO offset; alloc preserves X
        lda     #12
        jsr     alloc
        lda     #T_THUNK
        sta     (obj)
        lda     #0
        ldy     #2
        sta     (obj),y
        lda     CHUNK_INFO,x
        ldy     #4
        sta     (obj),y
        lda     CHUNK_INFO+2,x
        ldy     #6
        sta     (obj),y
        lda     env
        ldy     #8
        sta     (obj),y
        beq     @z
        lda     #$8000
@z:     ldy     #10
        sta     (obj),y
        jsr     push_p0
        jmp     run

op_force:
        jsr     force_top
        jmp     run

op_return:
        rts

; Force the value on top of the stack, in place. On return the top is a value.
force_top:
        jsr     peek_p0
@loop:  jsr     deref
        lda     (t0)
        and     #$00FF
        cmp     #T_THUNK
        beq     @thunk
        cmp     #T_INDIRECT
        beq     @indirect
        cmp     #T_BLACKHOLE
        beq     @infrec
        rts
@indirect:
        ldy     #4
        lda     (t0),y
        sta     p0
        iny
        iny
        lda     (t0),y
        sta     p0+2
        jsr     poke_p0
        bra     @loop
@infrec:
        lda     #E_INFREC
        jmp     err_exit
@thunk:
        tsc
        cmp     #CPU_STACK_MIN
        jcc     @deep
        sep     #$20
        .a8
        lda     #T_BLACKHOLE    ; re-entry during evaluation is infinite recursion
        sta     (t0)
        rep     #$20
        .a16
        lda     pc
        pha
        lda     ctab
        pha
        lda     env
        pha
        lda     t0
        pha
        ldy     #4
        lda     (t0),y
        sta     pc
        ldy     #6
        lda     (t0),y
        sta     ctab
        ldy     #8
        lda     (t0),y
        sta     env
        jsr     run             ; leaves the thunk's value above the thunk pointer
        pla
        sta     t0
        pla
        sta     env
        pla
        sta     ctab
        pla
        sta     pc
        jsr     pop_p0          ; the value
        lda     p0+2
        bpl     @deep2          ; a mature result is not something slice 1 makes
        lda     p0
        sta     t1
        ; in-place update: a value of 12 bytes or fewer overwrites the thunk
        lda     (t1)
        and     #$00FF
        cmp     #T_INT
        beq     @copy12
        cmp     #T_BOOL
        beq     @copy8
        cmp     #T_NULL
        beq     @copy8
        ; anything larger: the thunk becomes an INDIRECT to it
        lda     #T_INDIRECT
        sta     (t0)
        lda     #0
        ldy     #2
        sta     (t0),y
        ldy     #4
        lda     p0
        sta     (t0),y
        iny
        iny
        lda     p0+2
        sta     (t0),y
        bra     @again
@copy12:
        ldy     #10
        lda     (t1),y
        sta     (t0),y
        ldy     #8
        lda     (t1),y
        sta     (t0),y
@copy8: ldy     #6
        lda     (t1),y
        sta     (t0),y
        ldy     #4
        lda     (t1),y
        sta     (t0),y
        ldy     #2
        lda     (t1),y
        sta     (t0),y
        lda     (t1)
        sta     (t0)
@again: jsr     peek_p0         ; the thunk pointer, now pointing at a value
        jmp     @loop
@deep:  lda     #E_NOMEM
        jmp     err_exit
@deep2: lda     #E_TYPE
        jmp     err_exit

; ===========================================================================
; The shared singletons. Immutable: FORCE never touches a value, and nothing
; else writes through a pointer it did not just allocate.
obj_true:       .byte   T_BOOL, 0, 0, 0,  1, 0, 0, 0
obj_false:      .byte   T_BOOL, 0, 0, 0,  0, 0, 0, 0
obj_null:       .byte   T_NULL, 0, 0, 0,  0, 0, 0, 0
