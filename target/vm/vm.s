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
;   $0800 RSTACK     root stack            $0B00 CATCH    tryEval frames
;   $0C00 FSTACK     thunks under evaluation
;   $0E00 CHUNK_INFO 4 bytes/chunk: code address, constant-table address (128 chunks)
;   $1000 CTAB       constant pointer tables         $1800 CPU stack (grows down to $1840)
;   $2000 this program, 12 KB at most -- vm.cfg makes the linker refuse more,
;         because the first time it grew past $4000 the nursery silently
;         overwrote the true/false singletons and `n == 0` became a type error
;         at recursion depth 14
;   $5000 NURSERY (16 KB)    $9000 the .nbc image (8 KB max)    $B000 value stack (4 KB)
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
.macro  jmi  target
        bpl     :+
        jmp     target
:
.endmacro

RESULT          = $0300
RESULT_STAT     = $0320
RESULT_ERR      = $0321

CHUNK_INFO      = $0E00         ; CHUNK_MAX chunks
CTAB            = $1000
CTAB_END        = $1800
CPU_STACK_MIN   = $1840         ; FORCE refuses to nest below this
CPU_STACK       = $1FFF
RSTACK          = $0800         ; the root stack: 32-bit object pointers that
RSTACK_END      = $0B00         ;   frames hold across bytecode (HEAP.md, "roots")
CATCH           = $0B00         ; tryEval catch frames: sp, vsp, env(4), pc, ctab,
CATCH_MAX       = 12            ;   fstack, rsp -- 16 bytes each, up to $0BC0
FSTACK          = $0C00         ; the thunks being forced, innermost last, 32-bit
FSTACK_END      = $0E00         ;   pointers: a caught throw un-blackholes them
CHUNK_MAX       = 128           ; CHUNK_INFO at $0E00-$0FFF
NURSERY         = $5000
NURSERY_END     = $9000
NBC_BASE        = $9000
NBC_MAX         = $2000
VSTACK          = $B000
VSTACK_END      = $C000

T_INT           = $01
T_BOOL          = $02
T_NULL          = $03
T_STRING        = $04
T_LIST          = $06
T_ATTRS         = $07
T_THUNK         = $08
T_LAMBDA        = $09
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
env:            .res 4          ; pointer to the current env LIST (young or mature); 0 = none
vsp:            .res 2
rsp:            .res 2          ; bytes used in RSTACK (4 per pointer)
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
; slice 2: strings, lists, attribute sets, primops (data.s / prims.s)
p2:             .res 4          ; a third 32-bit value
t4:             .res 2
t5:             .res 2
t6:             .res 2
n0:             .res 2          ; element counts
n1:             .res 2
ix:             .res 2          ; loop indices
jx:             .res 2
kx:             .res 2
h:              .res 4          ; djb2 hash
hc:             .res 4          ; hash_t0's private scratch: it must not touch
hn:             .res 2          ;   p2 or kx, which its callers hold values in
slen:           .res 2          ; string length in flight
sav:            .res 2
catch_n:        .res 2          ; live tryEval frames
fstack_n:       .res 2          ; bytes used in FSTACK (2 per thunk)

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
        stz     env+2
        stz     rsp
        stz     catch_n
        stz     fstack_n

        jsr     load
        jsr     run             ; the entry chunk; RETURN brings us back here

        ; --- report the result: deep-forced, by pointer (BYTECODE.md) ---
        jsr     deep_force_top
        jsr     pop_p0
        lda     p0
        sta     RESULT
        lda     p0+2
        sta     RESULT+2
        sep     #$20
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
; from any nesting depth. A catchable error -- throw or a failed assert, the
; two Nix lets tryEval catch -- with a live catch frame unwinds to it instead
; (BYTECODE.md "Catch frames"): every register the frame saved is restored,
; including the CPU stack pointer, and tryEval resumes with the failure set.
err_exit:
        cmp     #E_THROW
        beq     @catchable
        cmp     #E_ASSERT
        bne     @fatal
@catchable:
        ldx     catch_n
        beq     @fatal
        dec     catch_n
        lda     catch_n
        asl
        asl
        asl
        asl                     ; * 16
        tax
        lda     CATCH+2,x
        sta     vsp
        lda     CATCH+4,x
        sta     env
        lda     CATCH+6,x
        sta     env+2
        lda     CATCH+8,x
        sta     pc
        lda     CATCH+10,x
        sta     ctab
        lda     CATCH+12,x
        sta     t2              ; fstack_n at the time of the tryEval
        lda     CATCH+14,x
        sta     rsp
        lda     CATCH+0,x
        tcs
        ; Every thunk that was under evaluation inside the tryEval was left
        ; BLACKHOLE by the unwind. Make them THUNKs again, so forcing one a
        ; second time re-runs it (and re-throws), as Nix would.
@unblack:
        lda     fstack_n
        cmp     t2
        beq     @unwound
        sec
        sbc     #4
        sta     fstack_n
        tax
        lda     FSTACK,x
        sta     p0
        lda     FSTACK+2,x
        sta     p0+2
        jsr     deref
        sep     #$20
        .a8
        lda     #T_THUNK
        sta     (t0)
        rep     #$20
        .a16
        bra     @unblack
@unwound:
        jmp     catch_resume
@fatal: sep     #$20
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
        cmp     #CHUNK_MAX+1
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
        jeq     @code
        dec     cnt
        lda     (q)
        and     #$00FF
        cmp     #K_INT
        beq     @int
        cmp     #K_STRING
        jne     badnbc
        ; STRING: u16 length, then the bytes
        ldy     #1
        lda     (q),y
        jsr     str_alloc       ; obj, p0, slen
        lda     q
        clc
        adc     #3
        tax
        lda     obj
        clc
        adc     #4
        tay
        lda     slen
        jsr     copy_bytes
        lda     q
        clc
        adc     #3
        clc
        adc     slen
        sta     q
        bra     @store
@int:   lda     #12
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
        lda     q
        clc
        adc     #9
        sta     q
@store: lda     cnext
        cmp     #CTAB_END
        jcs     nomem
        lda     p0
        sta     (cnext)
        ldy     #2
        lda     p0+2
        sta     (cnext),y
        lda     cnext
        clc
        adc     #4
        sta     cnext
        jmp     @const

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
        cmp     #OP_PRIM+1
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
        .word   op_jmp, op_jmp_if_false, op_jmp_if_true, op_assert       ; $20-$23
        .word   op_list, op_concat, op_attrs, op_select                  ; $24-$27
        .word   op_try_select, op_jmp_if_notfound                        ; $28-$29
        .word   op_has_attr, op_update, op_interpolate                   ; $2A-$2C
        .word   op_bad, op_bad, op_bad                                   ; $2D-$2F
        .word   op_env_new, op_env_set, op_env_get, op_env_pop           ; $30-$33
        .word   op_lambda, op_call                                       ; $34-$35
        .word   op_with_push, op_with_get, op_try_with_get, op_tail_call ; $36-$39
        .word   op_bad, op_bad, op_bad, op_bad, op_bad, op_bad           ; $3A-$3F
        .word   op_thunk, op_force, op_return, op_prim                   ; $40-$43

op_bad: lda     #E_BADOP
        jmp     err_exit

; The `x` register `run` dispatches with is OP*2; `jsr fetch8` above set
; the flags. RETURN of the entry chunk falls back to `start`. All of the
; handlers below `jmp run`; RETURN is `rts`.

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
        jsr     force_two
        jsr     need_int
        lda     (t1)
        and     #$00FF
        cmp     #T_INT
        jne     type_err
        rts

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
; ADD is polymorphic: two INTs add, two STRINGs concatenate.
op_add:
        jsr     force_two
        lda     (t0)
        and     #$00FF
        cmp     #T_STRING
        beq     @str
        cmp     #T_INT
        jne     type_err
        lda     (t1)
        and     #$00FF
        cmp     #T_INT
        jne     type_err
        jmp     add_ints
@str:   lda     (t1)
        and     #$00FF
        cmp     #T_STRING
        jne     type_err
        jsr     str_concat      ; t0 ++ t1 -> obj/p0
        jsr     push_p0
        jmp     run

; t0 = a, t1 = b, both INT (a was verified; b's tag was checked above).
add_ints:
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

; Ordering is polymorphic like Nix's: two INTs, or two STRINGs by bytes.
; cmp_values: pops and forces both, A = 0 (a < b), 1 (equal), 2 (a > b).
cmp_values:
        jsr     force_two
        lda     (t0)
        and     #$00FF
        sta     t2
        lda     (t1)
        and     #$00FF
        cmp     t2
        jne     type_err
        cmp     #T_INT
        jeq     cmp64
        cmp     #T_STRING
        jeq     str_cmp
        cmp     #T_LIST
        jeq     cmp_lists
        jmp     type_err

; cmp_lists: LISTs t0 (a) and t1 (b), lexicographic by element with the same
; ordering recursively; a proper prefix is less. a and b on the root stack
; (rbase+0, rbase+4); i 3,s  rbase 1,s.
cmp_lists:
        lda     t0
        sta     p0
        lda     #$8000
        sta     p0+2
        jsr     rpush_p0        ; a
        lda     t1
        sta     p0
        jsr     rpush_p0        ; b
        lda     #0
        pha
        lda     rsp
        sec
        sbc     #8
        pha
@loop:  lda     1,s
        tax
        jsr     rget_x
        jsr     deref
        jsr     aux_t0
        sta     n0              ; |a|
        lda     1,s
        clc
        adc     #4
        tax
        jsr     rget_x
        jsr     deref
        jsr     aux_t0
        sta     n1              ; |b|
        lda     3,s
        cmp     n0
        beq     @a_end
        lda     3,s
        cmp     n1
        beq     @greater        ; b is a proper prefix of a
        ; Nix skips equal prefixes with `==` (so pointer-equal functions pass)
        ; and orders by `<` only at the first difference.
        jsr     @elems          ; p0 = a[i], p1 = b[i]
        jsr     values_equal    ; runs bytecode
        bne     @differ
        lda     3,s
        inc     a
        sta     3,s
        bra     @loop
@differ:
        jsr     @elems
        lda     p1
        pha
        lda     p1+2
        pha
        jsr     push_p0
        pla
        sta     p0+2
        pla
        sta     p0
        jsr     push_p0
        jsr     cmp_values      ; pops both; runs bytecode
        bra     @decided
@a_end: lda     3,s
        cmp     n1
        beq     @equal
        lda     #0              ; a is a proper prefix of b
        bra     @decided
@greater:
        lda     #2
        bra     @decided
@equal: lda     #1
@decided:
        sta     t2
        pla
        sta     rsp
        pla
        lda     t2
        rts
; (a subroutine: its caller's frame is 2 bytes deeper)
@elems: lda     3,s
        tax
        jsr     rget_x
        jsr     deref
        lda     5,s
        jsr     elem_t0
        lda     p0
        sta     p2
        lda     p0+2
        sta     p2+2
        lda     3,s
        clc
        adc     #4
        tax
        jsr     rget_x
        jsr     deref
        lda     5,s
        jsr     elem_t0
        lda     p0
        sta     p1
        lda     p0+2
        sta     p1+2
        lda     p2
        sta     p0
        lda     p2+2
        sta     p0+2
        rts

op_lt:  jsr     cmp_values
        cmp     #0
        jsr     push_bool_z
        jmp     run
op_gt:  jsr     cmp_values
        cmp     #2
        jsr     push_bool_z
        jmp     run
op_le:  jsr     cmp_values
        cmp     #2              ; a <= b unless a > b
        beq     @false
        lda     #obj_true
        jsr     push_static
        jmp     run
@false: lda     #obj_false
        jsr     push_static
        jmp     run
op_ge:  jsr     cmp_values
        cmp     #0
        beq     @f
        lda     #0
        bra     @p
@f:     lda     #1
@p:     cmp     #0
        jsr     push_bool_z     ; Z set unless a < b
        jmp     run

; --- equality ------------------------------------------------------------------
; a b -> bool. The comparison itself is values_equal (data.s), which deep-forces
; lists and sets element-wise as Nix does.
; Two functions compared at the TOP level are never equal, even the same
; object -- C++ Nix's rule, which the corpus pins (eval-okay-cppnix-value-
; pointer-equality-encapsulation). Inside a list or set the pointer shortcut in
; values_equal applies, so `[ f ] == [ f ]` is true.
op_eq:
        jsr     force_two       ; t0 = a, t1 = b, both values
        lda     (t0)
        and     #$00FF
        cmp     #T_LAMBDA
        bne     @go
        lda     (t1)
        and     #$00FF
        cmp     #T_LAMBDA
        bne     @go
        lda     #obj_false
        jsr     push_static
        jmp     run
@go:    lda     t0
        sta     p0
        lda     #$8000
        sta     p0+2
        lda     t1
        sta     p1
        lda     #$8000
        sta     p1+2
        jsr     values_equal    ; p0 == p1 -> Z set if equal
        jsr     push_bool_z
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
        ldy     #4              ; [0] = the old env pointer, or 0
        lda     env
        sta     (obj),y
        iny
        iny
        lda     env+2
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
        lda     #$8000
        sta     env+2
        jmp     run

; The current environment, dereferenced: t0. (A mature env is a copy window
; once promotion exists; ENV_SET into one is the write barrier's job.)
env_t0: lda     env
        sta     p0
        lda     env+2
        sta     p0+2
        jmp     deref

op_env_set:
        jsr     fetch16
        asl
        asl
        clc
        adc     #4
        sta     t1              ; byte offset of element i (pop_p0 uses Y)
        jsr     pop_p0
        lda     p0
        sta     p1
        lda     p0+2
        sta     p1+2
        jsr     env_t0
        ldy     t1
        lda     p1
        sta     (t0),y
        iny
        iny
        lda     p1+2
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
        jsr     env_t0
@walk:  lda     t2
        beq     @got
        dec     t2
        ldy     #4
        lda     (t0),y          ; the parent pointer
        sta     p0
        iny
        iny
        lda     (t0),y
        sta     p0+2
        ora     p0
        beq     badenv
        jsr     deref
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
        jsr     env_t0
        ldy     #4
        lda     (t0),y
        sta     env
        iny
        iny
        lda     (t0),y
        sta     env+2
        jmp     run

; --- assert ---------------------------------------------------------------------------
op_assert:
        jsr     force_top
        jsr     pop_p0
        jsr     deref
        jsr     need_bool
        ldy     #4
        lda     (t0),y
        and     #$00FF
        beq     @fail
        jmp     run
@fail:  lda     #E_ASSERT
        jmp     err_exit

; --- closures: thunks and lambdas -----------------------------------------------------
; Both are 12 bytes -- header, code address + constant table (the "code
; pointer"), and the current environment (the "env pointer"). A lambda is a
; thunk that takes a parameter: CALL gives it an environment with the argument
; in slot 1 where FORCE would give a thunk its captured environment directly.
op_lambda:
        lda     #T_LAMBDA
        bra     closure
op_thunk:
        lda     #T_THUNK
closure:
        sta     t2
        jsr     fetch16
        asl
        asl
        tax                     ; CHUNK_INFO offset; alloc preserves X
        lda     #12
        jsr     alloc
        lda     t2
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
        lda     env+2
        ldy     #10
        sta     (obj),y
        jsr     push_p0
        jmp     run

op_force:
        jsr     force_top
        jmp     run

op_return:
        rts

; CALL: f a -> v. The callee's environment is a two-element LIST: [0] = the
; closure's captured environment, [1] = the argument, unforced.
op_call:
        jsr     do_call
        jmp     run

; do_call: the function and its argument are the top two values of the stack
; (argument on top). On return both are replaced by the call's value. Primops
; use this to apply Nix functions from native code (prims.s, call_with). The
; function is forced IN PLACE first: forcing runs bytecode, which may clobber
; any zero-page scratch, so the argument stays on the stack until then.
do_call:
        lda     pc              ; frame
        pha
        lda     ctab
        pha
        jsr     rpush_env       ; a root
        jsr     call_enter
        jsr     run             ; leaves the value where f was
        jsr     rpop_env
        pla
        sta     ctab
        pla
        sta     pc
        rts

; TAIL_CALL: enter the callee in this frame, so its RETURN returns to our
; caller and the CPU stack does not grow.
op_tail_call:
        jsr     call_enter
        jmp     run

; call_enter: f and its argument (on top) -> pc/ctab/env of the callee, both
; popped. A set with a __functor attribute is called through it:
; `s x` == `(s.__functor s) x`.
call_enter:
        lda     #2
        jsr     slot_addr
        sta     t6
        jsr     force_slot      ; f, forced in place
        jsr     pop_p0          ; the argument, kept in p1 across alloc
        lda     p0
        sta     p1
        lda     p0+2
        sta     p1+2
        jsr     pop_p0
        jsr     deref           ; t0 = the lambda
        lda     (t0)
        and     #$00FF
        cmp     #T_LAMBDA
        beq     @lambda
        cmp     #T_ATTRS
        jne     type_err
        lda     p0
        pha                     ; the set's pointer (p0 after pop/deref)
        lda     p0+2
        pha
        lda     p1
        sta     p0
        lda     p1+2
        sta     p0+2
        jsr     rpush_p0        ; the argument: a root across the functor call
        lda     #str_functor
        sta     t1
        jsr     find_attr
        jcc     type_err
        lda     ix
        jsr     attr_value      ; the functor, unforced
        jsr     push_p0
        pla
        sta     p0+2
        pla
        sta     p0
        jsr     push_p0         ; the set, as its own first argument
        jsr     do_call         ; g = s.__functor s
        jsr     rpop_p0
        jsr     push_p0         ; then g x
        jmp     call_enter
@lambda:
        tsc
        cmp     #CPU_STACK_MIN
        jcc     nomem
        lda     #12
        jsr     alloc           ; obj = the new environment
        lda     #(T_LIST | (2 << 8))
        sta     (obj)
        lda     #0
        ldy     #2
        sta     (obj),y
        ldy     #8              ; [0] = f.env, copied as the 32-bit pointer it is
        lda     (t0),y
        ldy     #4
        sta     (obj),y
        ldy     #10
        lda     (t0),y
        ldy     #6
        sta     (obj),y
        lda     p1              ; [1] = the argument
        ldy     #8
        sta     (obj),y
        lda     p1+2
        ldy     #10
        sta     (obj),y
        ldy     #4
        lda     (t0),y
        sta     pc
        ldy     #6
        lda     (t0),y
        sta     ctab
        lda     obj
        sta     env
        lda     #$8000
        sta     env+2
        rts

; --- with ------------------------------------------------------------------------------
; WITH_PUSH: the set (unforced) becomes an environment with the with mark.
op_with_push:
        jsr     pop_p0
        lda     p0
        sta     p1
        lda     p0+2
        sta     p1+2
        lda     #12
        jsr     alloc
        lda     #(T_LIST | (2 << 8))
        sta     (obj)
        lda     #$4000          ; aux bit 22: a with environment
        ldy     #2
        sta     (obj),y
        ldy     #4
        lda     env
        sta     (obj),y
        iny
        iny
        lda     env+2
        sta     (obj),y
        ldy     #8
        lda     p1
        sta     (obj),y
        iny
        iny
        lda     p1+2
        sta     (obj),y
        lda     obj
        sta     env
        lda     #$8000
        sta     env+2
        jmp     run

; WITH_GET / TRY_WITH_GET: k -> v. Walk the environment chain; at each with
; environment force its set and look k up. Frame: cur 1,s  key 3,s  strict 5,s.
op_with_get:
        lda     #1
        bra     with_get
op_try_with_get:
        lda     #0
; Frame: strict flag on the CPU stack (3,s), rbase (1,s); the key and the
; current environment on the root stack at rbase+0 and rbase+4.
with_get:
        pha
        jsr     force_pop_t0
        jsr     need_string
        jsr     rpush_p0        ; the key
        jsr     rpush_env       ; cur
        lda     rsp
        sec
        sbc     #8
        pha                     ; rbase
@walk:  lda     1,s
        clc
        adc     #4
        tax
        jsr     rget_x          ; p0 = cur
        lda     p0
        ora     p0+2
        beq     @miss
        jsr     deref
        ldy     #2
        lda     (t0),y
        and     #$4000
        beq     @next
        ldy     #8
        lda     (t0),y
        sta     p0
        iny
        iny
        lda     (t0),y
        sta     p0+2
        jsr     push_p0
        jsr     force_top       ; runs bytecode; the frame is on the stacks
        jsr     pop_p0
        jsr     deref
        jsr     need_attrs
        lda     1,s
        tax
        jsr     rget_x          ; p0 = the key
        jsr     deref_p0_t1     ; t1 = the key (t0 untouched)
        jsr     find_attr
        bcc     @next
        lda     ix
        jsr     attr_value
        pla
        sta     rsp             ; drops key and cur
        pla
        jsr     push_p0
        jmp     run
@next:  lda     1,s
        clc
        adc     #4
        tax
        jsr     rget_x
        jsr     deref
        ldy     #4
        lda     (t0),y
        sta     p0
        iny
        iny
        lda     (t0),y
        sta     p0+2
        lda     1,s
        clc
        adc     #4
        tax
        jsr     rset_x          ; cur = parent
        bra     @walk
@miss:  pla
        sta     rsp
        pla                     ; the strict flag
        bne     @novar
        stz     p0
        stz     p0+2
        jsr     push_p0
        jmp     run
@novar: lda     #E_NOVAR
        jmp     err_exit

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
        ldx     fstack_n        ; remember it, for a caught throw to un-blackhole
        cpx     #(FSTACK_END-FSTACK)
        jcs     @deep
        lda     p0              ; p0 is still the thunk's pointer (peek/poke)
        sta     FSTACK,x
        lda     p0+2
        sta     FSTACK+2,x
        inx
        inx
        inx
        inx
        stx     fstack_n
        lda     pc
        pha
        lda     ctab
        pha
        jsr     rpush_env       ; the caller's environment, a root
        ldy     #4
        lda     (t0),y
        sta     pc
        ldy     #6
        lda     (t0),y
        sta     ctab
        ldy     #8
        lda     (t0),y
        sta     env
        ldy     #10
        lda     (t0),y
        sta     env+2
        jsr     run             ; leaves the thunk's value above the thunk pointer
        jsr     rpop_env
        pla
        sta     ctab
        pla
        sta     pc
        lda     fstack_n        ; the thunk, by its (possibly updated) pointer
        sec
        sbc     #4
        sta     fstack_n
        tax
        lda     FSTACK,x
        sta     p1
        lda     FSTACK+2,x
        sta     p1+2
        jsr     pop_p0          ; the value
        lda     p1
        pha
        lda     p1+2
        pha
        jsr     deref_p1        ; t1 = the thunk
        lda     t1
        sta     t0
        pla
        sta     p1+2
        pla
        sta     p1
        lda     p0+2
        bpl     @deep2          ; a mature result is not something slice 1 makes
        lda     p0
        sta     t1
        ; in-place update: a value of 12 bytes or fewer overwrites the thunk.
        ; A THUNK result (a primop returning an unforced argument) is copied
        ; too, and the loop below then forces it.
        lda     (t1)
        and     #$00FF
        cmp     #T_INT
        beq     @copy12
        cmp     #T_LAMBDA
        beq     @copy12
        cmp     #T_THUNK
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
@again: lda     p1              ; the thunk pointer (it may have been updated by
        sta     p0              ; a collection while the body ran), now a value
        lda     p1+2
        sta     p0+2
        jsr     poke_p0
        jmp     @loop
@deep:  lda     #E_NOMEM
        jmp     err_exit
@deep2: lda     #E_TYPE
        jmp     err_exit

; ===========================================================================
        .include "data.s"       ; strings, lists, attribute sets
        .include "equal.s"      ; structural equality, deep force
        .include "prims.s"      ; the primop table

; ===========================================================================
; The shared singletons. Immutable: FORCE never touches a value, and nothing
; else writes through a pointer it did not just allocate.
obj_true:       .byte   T_BOOL, 0, 0, 0,  1, 0, 0, 0
obj_false:      .byte   T_BOOL, 0, 0, 0,  0, 0, 0, 0
obj_null:       .byte   T_NULL, 0, 0, 0,  0, 0, 0, 0
