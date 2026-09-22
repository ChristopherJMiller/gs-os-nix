; ---------------------------------------------------------------------------
; nursery.s -- the CPU-side nursery: bump allocation in fast RAM, a Cheney
; collection inside RAM, and promotion of survivors to the card.
;
; This is where D6 ("the generational boundary is the slot connector") becomes
; code. Objects are born here at fast-RAM speed and most die here; only what
; survives a collection is ever pushed across the 1.024 MHz bus.
;
; Layout (docs/HEAP.md, identical to the card's): 4-byte header, payload,
; 32-bit little-endian pointers. A pointer into the nursery has BIT 31 SET --
; the card's 1 MB heap never uses that bit, so the two spaces cannot be confused.
;
; The rule that keeps this simple: a mature (card-resident) object never points
; into the nursery, so there is no remembered set. Young -> old is free.
;
; Promotion is one DATA burst. A collection copies survivors into a contiguous
; staging area; RESERVE claims the same number of bytes on the card; the staging
; area is streamed across with every young pointer translated by the constant
;     card = base + (addr - STAGE)
; which holds because Cheney preserves order and sizes.
;
; Subset: sizes/pointer maps for INT, LIST, THUNK (the rest take 8 bytes, no
; pointers). Objects under 256 bytes. Emulation mode, 8-bit -- a2min runs this;
; the native-mode version arrives with the VM at L4. The MEASUREMENT (bytes
; crossing the bus) does not depend on CPU mode.
;
; Include after defining SLOT_X (slot*$10) and with these zeropage symbols
; available. Card in slot 4 by default.
; ---------------------------------------------------------------------------

NURSERY         = $4000         ; from-space: objects are born here
NURSERY_END     = $6000
STAGE           = $6000         ; staging: survivors land here, contiguous
STAGE_END       = $8000

T_INT           = $01
T_LIST          = $06
T_THUNK         = $08
T_FORWARDED     = $0C

REG_CMD         = $C080
REG_CTRL        = $C081
REG_ASEL        = $C082
REG_APORT       = $C083
REG_DATA        = $C084
REG_LEN         = $C088
V_RESERVE       = $25
ST_DONE         = $40
ST_ERR          = $20

; Zero-page state is declared by nursery_zp.inc, which the including program must
; pull in BEFORE its first reference (see that file for why).

        .code

; ---------------------------------------------------------------------------
n_init:
        lda     #<NURSERY
        sta     nfree
        lda     #>NURSERY
        sta     nfree+1
        lda     #<STAGE
        sta     stage
        lda     #>STAGE
        sta     stage+1
        lda     #0
        sta     allocd
        sta     allocd+1
        sta     nobj
        sta     nobj+1
        sta     nprom
        sta     nprom+1
        rts

; ---------------------------------------------------------------------------
; n_alloc -- A = total object size in bytes (header included, already rounded,
; >= 8). Returns the object's address in `obj`. The caller writes the header.
; No automatic collection: the caller decides when (the VM will, on overflow).
; ---------------------------------------------------------------------------
n_alloc:
        sta     osize
        lda     nfree
        sta     obj
        lda     nfree+1
        sta     obj+1
        clc
        lda     nfree
        adc     osize
        sta     nfree
        lda     nfree+1
        adc     #0
        sta     nfree+1
        clc
        lda     allocd
        adc     osize
        sta     allocd
        lda     allocd+1
        adc     #0
        sta     allocd+1
        inc     nobj
        bne     :+
        inc     nobj+1
:       rts

; ---------------------------------------------------------------------------
; obj_size_at_obj -- sets tag, aux, osize for the object at `obj`.
; ---------------------------------------------------------------------------
obj_size_at_obj:
        ldy     #0
        lda     (obj),y
        sta     tag
        iny
        lda     (obj),y
        sta     aux
        lda     tag
        cmp     #T_INT
        beq     @twelve
        cmp     #T_THUNK
        beq     @twelve
        cmp     #T_LIST
        beq     @list
        lda     #8
        sta     osize
        rts
@twelve:
        lda     #12
        sta     osize
        rts
@list:                          ; 4 + 4*count
        lda     aux
        asl     a
        asl     a
        clc
        adc     #4
        sta     osize
        rts

; ---------------------------------------------------------------------------
; nptr_of_tag -- sets nptr = number of leading payload words that are pointers.
; LIST: all `count`; THUNK: 2; everything else in the subset: 0.
; ---------------------------------------------------------------------------
nptr_of_tag:
        lda     tag
        cmp     #T_LIST
        beq     @l
        cmp     #T_THUNK
        beq     @t
        lda     #0
        sta     nptr
        rts
@l:     lda     aux
        sta     nptr
        rts
@t:     lda     #2
        sta     nptr
        rts

; ---------------------------------------------------------------------------
; evac_at -- the 32-bit pointer at (src) is evacuated in place.
; Not young: untouched. Forwarded: replaced by the forwarding pointer. Else the
; object is copied to staging, a forwarding pointer left behind, and (src)
; updated to point at the copy (still young: staging is in RAM).
; Clobbers obj, tag, aux, osize.
; ---------------------------------------------------------------------------
evac_at:
        ldy     #3
        lda     (src),y
        and     #$80
        bne     :+
        rts                     ; a mature (card) pointer, or null
:
        ldy     #0
        lda     (src),y
        sta     obj
        iny
        lda     (src),y
        sta     obj+1

        ldy     #0
        lda     (obj),y
        cmp     #T_FORWARDED
        bne     @copy

        ; Already moved: (src) <- forwarding pointer at obj+4..7
        ldy     #4
@fw:    lda     (obj),y
        pha
        tya
        sec
        sbc     #4
        tay
        pla
        sta     (src),y
        tya
        clc
        adc     #5
        tay
        cpy     #8
        bne     @fw
        rts

@copy:
        jsr     obj_size_at_obj
        ldy     #0
@cl:    lda     (obj),y
        sta     (stage),y
        iny
        cpy     osize
        bne     @cl

        ; forwarding pointer: obj[0] = FORWARDED, obj[4..7] = young(stage)
        lda     #T_FORWARDED
        ldy     #0
        sta     (obj),y
        ldy     #4
        lda     stage
        sta     (obj),y
        iny
        lda     stage+1
        sta     (obj),y
        iny
        lda     #0
        sta     (obj),y
        iny
        lda     #$80
        sta     (obj),y

        ; (src) = young(stage)
        ldy     #0
        lda     stage
        sta     (src),y
        iny
        lda     stage+1
        sta     (src),y
        iny
        lda     #0
        sta     (src),y
        iny
        lda     #$80
        sta     (src),y

        ; stage += osize
        clc
        lda     stage
        adc     osize
        sta     stage
        lda     stage+1
        adc     #0
        sta     stage+1
        rts

; ---------------------------------------------------------------------------
; n_collect -- Cheney collection of the nursery into staging.
; Roots: `nroots` 32-bit pointers at ROOTS, updated in place.
; ---------------------------------------------------------------------------
n_collect:
        lda     #<STAGE
        sta     stage
        sta     scan
        lda     #>STAGE
        sta     stage+1
        sta     scan+1

        ; --- roots ---
        lda     #<ROOTS
        sta     src
        lda     #>ROOTS
        sta     src+1
        lda     nroots
        sta     cnt
@rl:    lda     cnt
        beq     @scan
        jsr     evac_at
        clc
        lda     src
        adc     #4
        sta     src
        lda     src+1
        adc     #0
        sta     src+1
        dec     cnt
        bra     @rl

        ; --- Cheney scan: everything below `scan` is fully traced ---
@scan:
        lda     scan
        cmp     stage
        bne     @go
        lda     scan+1
        cmp     stage+1
        bne     @go
        rts                     ; scan caught up with stage: done
@go:
        lda     scan
        sta     obj
        sta     cur
        lda     scan+1
        sta     obj+1
        sta     cur+1
        jsr     obj_size_at_obj
        jsr     nptr_of_tag
        lda     nptr
        sta     cnt
        ; src = cur + 4
        clc
        lda     cur
        adc     #4
        sta     src
        lda     cur+1
        adc     #0
        sta     src+1
@fl:    lda     cnt
        beq     @adv
        jsr     evac_at         ; clobbers obj/tag/aux/osize -- cur is safe
        clc
        lda     src
        adc     #4
        sta     src
        lda     src+1
        adc     #0
        sta     src+1
        dec     cnt
        bra     @fl
@adv:
        ; recompute this object's size (evac clobbered osize) and advance scan
        lda     cur
        sta     obj
        lda     cur+1
        sta     obj+1
        jsr     obj_size_at_obj
        clc
        lda     scan
        adc     osize
        sta     scan
        lda     scan+1
        adc     #0
        sta     scan+1
        bra     @scan

; ---------------------------------------------------------------------------
; Card helpers. X must hold the slot index (slot * $10) on entry and is
; preserved; Y is used freely.
; ---------------------------------------------------------------------------
card_wait:
@w:     lda     REG_CMD,x
        and     #ST_DONE|ST_ERR
        beq     @w
        rts

; LEN <- tmp (4 bytes); rewinds the port index first (CARD-ISA.md, CTRL b3).
card_set_len_tmp:
        lda     #$08
        sta     REG_CTRL,x
        lda     tmp
        sta     REG_LEN,x
        lda     tmp+1
        sta     REG_LEN,x
        lda     tmp+2
        sta     REG_LEN,x
        lda     tmp+3
        sta     REG_LEN,x
        rts

; xbase <- A1
card_get_a1:
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
        rts

; A0 <- xbase, so DATA writes stream into the reserved block.
card_set_a0_xbase:
        lda     #0
        sta     REG_ASEL,x
        lda     xbase
        sta     REG_APORT,x
        lda     xbase+1
        sta     REG_APORT,x
        lda     xbase+2
        sta     REG_APORT,x
        lda     xbase+3
        sta     REG_APORT,x
        rts

; ---------------------------------------------------------------------------
; translate -- tmp (a young 32-bit pointer) <- xbase + (addr16 - STAGE)
; ---------------------------------------------------------------------------
translate:
        sec
        lda     tmp
        sbc     #<STAGE
        sta     tmp
        lda     tmp+1
        sbc     #>STAGE
        sta     tmp+1
        clc
        lda     tmp
        adc     xbase
        sta     tmp
        lda     tmp+1
        adc     xbase+1
        sta     tmp+1
        lda     #0
        adc     xbase+2
        sta     tmp+2
        lda     #0
        adc     xbase+3
        sta     tmp+3
        rts

; ---------------------------------------------------------------------------
; n_promote -- push the staging area to the card as one DATA burst, translating
; young pointers on the way; then translate the roots; then reset the nursery.
; Leaves the promoted byte count in tmp (16 bits) and the block base in xbase.
; ---------------------------------------------------------------------------
n_promote:
        ; total = stage - STAGE  -> tmp, zero-extended to 32 bits
        sec
        lda     stage
        sbc     #<STAGE
        sta     tmp
        lda     stage+1
        sbc     #>STAGE
        sta     tmp+1
        lda     #0
        sta     tmp+2
        sta     tmp+3
        pha                     ; keep total for the caller (2 bytes via zp later)
        lda     tmp
        pha
        lda     tmp+1
        pha

        jsr     card_set_len_tmp
        lda     #V_RESERVE
        sta     REG_CMD,x
        jsr     card_wait
        jsr     card_get_a1
        jsr     card_set_a0_xbase

        ; --- walk staging, emitting bytes ---
        lda     #<STAGE
        sta     cur
        lda     #>STAGE
        sta     cur+1
@obj:
        lda     cur
        cmp     stage
        bne     @emit
        lda     cur+1
        cmp     stage+1
        bne     @emit
        jmp     @roots          ; the emit block below is too long for a branch
@emit:
        lda     cur
        sta     obj
        lda     cur+1
        sta     obj+1
        jsr     obj_size_at_obj
        jsr     nptr_of_tag
        inc     nprom
        bne     :+
        inc     nprom+1
:
        ; header: 4 raw bytes
        ldy     #0
@h:     lda     (cur),y
        sta     REG_DATA,x
        iny
        cpy     #4
        bne     @h
        ; payload words
        lda     #0
        sta     cnt             ; word index
@w:     lda     cnt
        asl     a
        asl     a
        clc
        adc     #4
        cmp     osize
        bcc     @inrange
        jmp     @next           ; past the end (too far for a branch)
@inrange:
        tay                     ; Y = byte offset of this word
        lda     cnt
        cmp     nptr
        bcs     @raw            ; not a pointer field
        ; a pointer field: young?
        iny
        iny
        iny
        lda     (cur),y
        and     #$80
        beq     @rawy           ; mature or null: raw
        ; young: translate. tmp <- the 4 bytes, then translate, then emit tmp
        dey
        dey
        dey
        lda     (cur),y
        sta     tmp
        iny
        lda     (cur),y
        sta     tmp+1
        jsr     translate
        lda     tmp
        sta     REG_DATA,x
        lda     tmp+1
        sta     REG_DATA,x
        lda     tmp+2
        sta     REG_DATA,x
        lda     tmp+3
        sta     REG_DATA,x
        bra     @inc
@rawy:  dey
        dey
        dey
@raw:   lda     (cur),y
        sta     REG_DATA,x
        iny
        lda     (cur),y
        sta     REG_DATA,x
        iny
        lda     (cur),y
        sta     REG_DATA,x
        iny
        lda     (cur),y
        sta     REG_DATA,x
@inc:   inc     cnt
        bra     @w
@next:
        clc
        lda     cur
        adc     osize
        sta     cur
        lda     cur+1
        adc     #0
        sta     cur+1
        jmp     @obj            ; too far back for a branch

        ; --- roots: translate the young ones in place ---
@roots:
        lda     #<ROOTS
        sta     src
        lda     #>ROOTS
        sta     src+1
        lda     nroots
        sta     cnt
@rt:    lda     cnt
        beq     @reset
        ldy     #3
        lda     (src),y
        and     #$80
        beq     @rn
        ldy     #0
        lda     (src),y
        sta     tmp
        iny
        lda     (src),y
        sta     tmp+1
        jsr     translate
        ldy     #0
        lda     tmp
        sta     (src),y
        iny
        lda     tmp+1
        sta     (src),y
        iny
        lda     tmp+2
        sta     (src),y
        iny
        lda     tmp+3
        sta     (src),y
@rn:    clc
        lda     src
        adc     #4
        sta     src
        lda     src+1
        adc     #0
        sta     src+1
        dec     cnt
        bra     @rt

@reset:
        ; nursery is empty again
        lda     #<NURSERY
        sta     nfree
        lda     #>NURSERY
        sta     nfree+1
        lda     #<STAGE
        sta     stage
        lda     #>STAGE
        sta     stage+1
        ; restore total into tmp for the caller
        pla
        sta     tmp+1
        pla
        sta     tmp
        pla
        rts
