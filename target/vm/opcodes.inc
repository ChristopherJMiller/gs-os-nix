; target/vm/opcodes.inc -- the .nbc opcode table for the 65816 VM.
;
; docs/BYTECODE.md is normative. This file and tools/nbc/src/opcodes.rs must match
; it exactly; checks.nbc-opcodes (scripts/check-opcodes.py) parses all three and
; fails on any difference. Keep the `OP_NAME = $XX` form: the checker greps for it.

OP_CONST        = $01
OP_POP          = $02
OP_TRUE         = $03
OP_FALSE        = $04
OP_NULL         = $05

OP_ADD          = $10
OP_SUB          = $11
OP_MUL          = $12
OP_DIV          = $13
OP_NEG          = $14
OP_EQ           = $16
OP_LT           = $17
OP_LE           = $18
OP_GT           = $19
OP_GE           = $1A
OP_NOT          = $1B

OP_JMP          = $20
OP_JMP_IF_FALSE = $21
OP_JMP_IF_TRUE  = $22
OP_ASSERT       = $23

OP_ENV_NEW      = $30
OP_ENV_SET      = $31
OP_ENV_GET      = $32
OP_ENV_POP      = $33
OP_LAMBDA       = $34
OP_CALL         = $35

OP_THUNK        = $40
OP_FORCE        = $41
OP_RETURN       = $42

; Constant kinds are heap tags (docs/HEAP.md).
K_INT           = $01
K_STRING        = $04

; Error codes reported at $0321 when the status byte is $FF.
E_BADOP         = $01
E_TYPE          = $02
E_DIVZERO       = $03
E_INFREC        = $04
E_NOMEM         = $05
E_BADNBC        = $06
E_NOCARD        = $07
E_ASSERT        = $08
