; ---------------------------------------------------------------------------
; storepath_demo -- compute a Nix store path on an Apple IIgs, using the card.
;
; This is the M2 program (docs/STATUS.md): it drives the verb sequence from
; docs/CARD-ISA.md and prints the resulting 32-character store-path component.
; Success is that the printed string is byte-identical to what `nix` produces on
; a modern machine for the same fingerprint.
;
; Deliberately written with 8-bit ops in emulation mode. The card interface is
; byte-at-a-time, so native-mode 16-bit registers buy nothing here; they start
; earning their keep with the bytecode VM at L4. What this proves is the card.
;
; Register offsets are indexed by X = slot * $10, so a card in slot n decodes at
; $C080+X .. $C08F+X. See docs/CARD-ISA.md.
; ---------------------------------------------------------------------------

        .setcpu "65816"

; --- Apple II monitor entry points ---
COUT            = $FDED         ; print A as a character
CROUT           = $FD8E         ; print a carriage return

; --- Card registers, relative to $C080 + slot*$10 ---
REG_CMD         = $C080         ; W: verb        R: status
REG_CTRL        = $C081         ; W: control     R: id ($A2)
REG_DATA        = $C084         ; RW: data / hash push
REG_PSEL        = $C086         ; W: select parameter slot
REG_PPORT       = $C087         ; RW: parameter slot bytes

; --- Verbs ---
V_HASH_INIT     = $10
V_HASH_FINAL    = $12
V_HASH_PUSH     = $15

; --- Status bits ---
ST_DONE         = $40
ST_ERR          = $20
ST_READY        = $10

CARD_ID         = $A2
FIFO_BURST      = 16            ; bytes licensed by one READY (CARD-ISA.md)

; ---------------------------------------------------------------------------
        .zeropage
slot_x:         .res 1          ; slot * $10, the index into the register file
src_ptr:        .res 2          ; pointer into the fingerprint / inline string
remaining:      .res 1          ; fingerprint bytes left to push
burst:          .res 1          ; bytes left in the current burst

; ---------------------------------------------------------------------------
        .code

start:
        sec
        xce                     ; stay in 6502 emulation mode
        cld

        jsr     find_card
        bcs     have_card

        jsr     print_str
        .byte   "NO A2NIX CARD FOUND", $0D, $00
        rts

have_card:
        jsr     say_found
        jsr     hash_fingerprint
        bcc     hashed

        jsr     print_str
        .byte   "CARD REPORTED AN ERROR", $0D, $00
        rts

hashed:
        jmp     print_path

; ---------------------------------------------------------------------------
; find_card -- scan slots 7..1 for a card whose ID register reads $A2.
; Exit: carry set and slot_x valid on success; carry clear if not found.
; ---------------------------------------------------------------------------
find_card:
        ldx     #$70            ; slot 7 first
@loop:
        lda     REG_CTRL,x
        cmp     #CARD_ID
        beq     @found
        txa
        sec
        sbc     #$10
        tax
        bne     @loop           ; stop after slot 1 (X would reach $00)
        clc
        rts
@found:
        stx     slot_x
        sec
        rts

; ---------------------------------------------------------------------------
; hash_fingerprint -- push the fingerprint through HASH_PUSH, then finalise.
; Exit: carry clear on success; carry set if the card raised ERR.
; ---------------------------------------------------------------------------
hash_fingerprint:
        ldx     slot_x

        lda     #V_HASH_INIT    ; mandatory: abandons any partial message
        sta     REG_CMD,x
        lda     #V_HASH_PUSH
        sta     REG_CMD,x

        lda     #<fingerprint
        sta     src_ptr
        lda     #>fingerprint
        sta     src_ptr+1
        lda     #fingerprint_len
        sta     remaining

@next_burst:
        lda     remaining
        beq     @finish

        ; One READY poll licenses FIFO_BURST writes. Polling per byte would also
        ; be correct, just ~2x the bus traffic (CARD-ISA.md economics).
@poll_ready:
        lda     REG_CMD,x
        and     #ST_ERR
        bne     @error
        lda     REG_CMD,x
        and     #ST_READY
        beq     @poll_ready

        lda     #FIFO_BURST     ; burst = min(FIFO_BURST, remaining)
        cmp     remaining
        bcc     @have_burst
        lda     remaining
@have_burst:
        sta     burst

        ldy     #0
@push:
        lda     (src_ptr),y
        sta     REG_DATA,x
        iny
        cpy     burst
        bne     @push

        tya                     ; src_ptr += burst
        clc
        adc     src_ptr
        sta     src_ptr
        bcc     @no_carry
        inc     src_ptr+1
@no_carry:
        lda     remaining       ; remaining -= burst
        sec
        sbc     burst
        sta     remaining
        bne     @next_burst

@finish:
        lda     #V_HASH_FINAL   ; drains the FIFO, then pads
        sta     REG_CMD,x
@poll_done:
        lda     REG_CMD,x
        and     #ST_ERR
        bne     @error
        lda     REG_CMD,x
        and     #ST_DONE
        beq     @poll_done

        clc
        rts
@error:
        sec
        rts

; ---------------------------------------------------------------------------
; print_path -- read the 32-character store path from P3 and print it.
; ---------------------------------------------------------------------------
print_path:
        ldx     slot_x
        lda     #3              ; P3 holds the nixbase32 path
        sta     REG_PSEL,x

        ldy     #0
@loop:
        ldx     slot_x          ; COUT does not preserve X
        lda     REG_PPORT,x     ; auto-increments the slot's byte index
        ora     #$80            ; the monitor wants high-bit ASCII
        jsr     COUT
        iny
        cpy     #32
        bne     @loop

        jmp     CROUT

; ---------------------------------------------------------------------------
say_found:
        jsr     print_str
        .byte   "A2NIX CARD IN SLOT ", $00
        lda     slot_x
        lsr     a               ; slot_x is slot*$10; recover the slot number
        lsr     a
        lsr     a
        lsr     a
        ora     #$B0            ; ASCII digit with the high bit set
        jsr     COUT
        jmp     CROUT

; ---------------------------------------------------------------------------
; print_str -- print the inline $00-terminated string following the JSR, then
; return to the byte after the terminator.
;
; RTS pulls PCL then PCH, so the resumed address must be pushed high byte first.
; ---------------------------------------------------------------------------
print_str:
        pla                     ; (return address - 1), low
        sta     src_ptr
        pla                     ; high
        sta     src_ptr+1

        ldy     #1              ; the string starts one past that
@loop:
        lda     (src_ptr),y
        beq     @done
        ora     #$80
        jsr     COUT
        iny
        bne     @loop
@done:
        tya                     ; src_ptr += y, landing on the terminator
        clc
        adc     src_ptr
        sta     src_ptr
        lda     src_ptr+1
        adc     #0
        pha                     ; push high first
        lda     src_ptr
        pha
        rts

; ---------------------------------------------------------------------------
; The fingerprint for `builtins.toFile "nix-on-an-apple-iigs"
;                      "the card does the arithmetic"`.
;
; Format (docs/REFERENCES.md):
;     text:sha256:<hex sha256 of contents>:/nix/store:<name>
;
; Expected output, which upstream nix agrees on:
;     fsl59pk3f4c65048v3w7vgazqwg4d75w
;
; Kept in sync with tests/vectors/l1.txt -- `just check-demo-vector` fails if
; this string and that file ever disagree.
; ---------------------------------------------------------------------------
        .rodata
fingerprint:
        .byte   "text:sha256:"
        .byte   "f56971ab18856ec4e508557d411953da"
        .byte   "034ef8c82f513aad5bd30fd4855431d7"
        .byte   ":/nix/store:nix-on-an-apple-iigs"
fingerprint_end:
fingerprint_len = fingerprint_end - fingerprint
