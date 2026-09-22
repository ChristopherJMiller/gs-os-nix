; Zero-page state for target/heap/nursery.s.
;
; Kept in a separate include so a program can declare it at the TOP of its source,
; before any reference: ca65 fixes addressing modes on first sight, and a zero-page
; symbol it has not yet seen is assembled with (slower, larger) absolute addressing.
; nursery.s itself is included at the bottom, after the program's entry point.

        .zeropage
nfree:          .res 2          ; nursery bump pointer
stage:          .res 2          ; staging bump pointer
scan:           .res 2          ; Cheney scan pointer
cur:            .res 2          ; object being scanned / walked
obj:            .res 2          ; object being evacuated
src:            .res 2          ; address of the 32-bit pointer being evacuated
osize:          .res 1          ; current object's size (< 256)
tag:            .res 1
aux:            .res 1          ; low byte of aux; enough for the subset
cnt:            .res 1          ; pointer fields left
nptr:           .res 1          ; pointer fields in the current object
xbase:          .res 4          ; card address of the reserved block
tmp:            .res 4
allocd:         .res 2          ; bytes allocated since init (the D6 numerator)
nobj:           .res 2          ; objects allocated
nprom:          .res 2          ; objects promoted
