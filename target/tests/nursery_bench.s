; ---------------------------------------------------------------------------
; nursery_bench -- allocate a workload in the nursery, keep a little of it live,
; collect, promote, and report how much crossed the bus.
;
; This is the D6 measurement. Lazy evaluation has extreme infant mortality; the
; claim is that almost nothing allocated ever reaches the 1.024 MHz slot. Here
; 50 objects (600 bytes) are born and two lists are kept alive by roots. Those
; two lists happen to reference the SAME two INTs (list i holds ints[i&3] and
; ints[(i+1)&3]; 3 and 7 agree mod 4), so the live set is 4 objects, 48 bytes,
; and the shared INTs must reach the card exactly once each -- sharing across
; separate roots is precisely what forwarding pointers exist for. If the design
; is right, 48 bytes cross the bus and 552 never do.
;
; Results at RESULT (all little-endian):
;   +0  allocated bytes (2)     +2  promoted bytes (2)
;   +4  objects allocated (2)   +6  objects promoted (2)
;   +8  card block base (4)     +12 ROOTS[0] after promotion (4)
;   $0320 status: $01 done
; The harness (a2min --bench-nursery) counts bus accesses itself and inspects the
; card heap at the block base to verify the promoted graph is intact.
; ---------------------------------------------------------------------------

        .setcpu "65816"

RESULT          = $0300
RESULT_STAT     = $0320
ROOTS           = $0380         ; up to 8 x 32-bit roots
CARD_ID_REG     = $C081
CARD_ID         = $A2

        .zeropage
nroots:         .res 1
i:              .res 1
slot_x:         .res 1
ints:           .res 8          ; addresses of the first four INTs, for lists

        ; the nursery's zero-page state, declared before first use
        .include "../heap/nursery_zp.inc"

        .code
start:
        sec
        xce
        cld

        ; find the card (ID $A2), slot 7 down to 1
        ldx     #$70
@scan:  lda     CARD_ID_REG,x
        cmp     #CARD_ID
        beq     @found
        txa
        sec
        sbc     #$10
        tax
        bne     @scan
        lda     #$FF
        sta     RESULT_STAT
        rts
@found: stx     slot_x

        jsr     n_init

        ; --- 40 INTs, payload = index. Remember the first four. ---
        lda     #0
        sta     i
@ints:
        lda     #12
        jsr     n_alloc
        ldy     #0
        lda     #T_INT
        sta     (obj),y
        iny
        lda     #0
        sta     (obj),y
        iny
        sta     (obj),y
        iny
        sta     (obj),y
        ldy     #4
        lda     i
        sta     (obj),y         ; payload low byte = index
        iny
        lda     #0
@z:     sta     (obj),y
        iny
        cpy     #12
        bne     @z
        lda     i
        cmp     #4
        bcs     @ni
        asl     a
        tay
        lda     obj
        sta     ints,y
        lda     obj+1
        sta     ints+1,y
@ni:    inc     i
        lda     i
        cmp     #40
        bne     @ints

        ; --- 10 LISTs of 2, elements = young pointers to ints[i%4], ints[(i+1)%4] ---
        lda     #0
        sta     i
@lists:
        lda     #12
        jsr     n_alloc
        ldy     #0
        lda     #T_LIST
        sta     (obj),y
        iny
        lda     #2              ; count
        sta     (obj),y
        iny
        lda     #0
        sta     (obj),y
        iny
        sta     (obj),y
        ; element 0: ints[i & 3]
        lda     i
        and     #3
        asl     a
        tax
        ldy     #4
        lda     ints,x
        sta     (obj),y
        iny
        lda     ints+1,x
        sta     (obj),y
        iny
        lda     #0
        sta     (obj),y
        iny
        lda     #$80            ; young
        sta     (obj),y
        ; element 1: ints[(i+1) & 3]
        lda     i
        clc
        adc     #1
        and     #3
        asl     a
        tax
        ldy     #8
        lda     ints,x
        sta     (obj),y
        iny
        lda     ints+1,x
        sta     (obj),y
        iny
        lda     #0
        sta     (obj),y
        iny
        lda     #$80
        sta     (obj),y
        ; lists 3 and 7 become roots
        lda     i
        cmp     #3
        beq     @root
        cmp     #7
        bne     @nl
@root:  lda     nroots
        asl     a
        asl     a
        tay
        lda     obj
        sta     ROOTS,y
        iny
        lda     obj+1
        sta     ROOTS,y
        iny
        lda     #0
        sta     ROOTS,y
        iny
        lda     #$80
        sta     ROOTS,y
        inc     nroots
@nl:    inc     i
        lda     i
        cmp     #10
        beq     @lists_done
        jmp     @lists          ; the loop body is too long for a branch
@lists_done:

        ; --- report allocation ---
        lda     allocd
        sta     RESULT+0
        lda     allocd+1
        sta     RESULT+1
        lda     nobj
        sta     RESULT+4
        lda     nobj+1
        sta     RESULT+5

        ; --- collect and promote ---
        ldx     slot_x
        jsr     n_collect
        ldx     slot_x
        jsr     n_promote

        lda     tmp
        sta     RESULT+2
        lda     tmp+1
        sta     RESULT+3
        lda     nprom
        sta     RESULT+6
        lda     nprom+1
        sta     RESULT+7
        ldy     #0
@xb:    lda     xbase,y
        sta     RESULT+8,y
        lda     ROOTS,y
        sta     RESULT+12,y
        iny
        cpy     #4
        bne     @xb

        lda     #1
        sta     RESULT_STAT
        rts

        .include "../heap/nursery.s"
