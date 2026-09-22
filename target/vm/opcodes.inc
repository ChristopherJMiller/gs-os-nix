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
OP_LIST         = $24
OP_CONCAT       = $25
OP_ATTRS        = $26
OP_SELECT       = $27
OP_TRY_SELECT   = $28
OP_JMP_IF_NOTFOUND = $29
OP_HAS_ATTR     = $2A
OP_UPDATE       = $2B
OP_INTERPOLATE  = $2C

OP_ENV_NEW      = $30
OP_ENV_SET      = $31
OP_ENV_GET      = $32
OP_ENV_POP      = $33
OP_LAMBDA       = $34
OP_CALL         = $35
OP_WITH_PUSH    = $36
OP_WITH_GET     = $37
OP_TRY_WITH_GET = $38
OP_TAIL_CALL    = $39

OP_THUNK        = $40
OP_FORCE        = $41
OP_RETURN       = $42
OP_PRIM         = $43

; Primops: PRIM_<NAME> = id ; arity. The Nix name is in BYTECODE.md's table;
; checks.nbc-opcodes matches these by upper-casing the name and dropping `'`.
PRIM_LENGTH             = $00 ; 1
PRIM_HEAD               = $01 ; 1
PRIM_TAIL               = $02 ; 1
PRIM_ELEMAT             = $03 ; 2
PRIM_MAP                = $04 ; 2
PRIM_FILTER             = $05 ; 2
PRIM_FOLDL              = $06 ; 3
PRIM_CONCATLISTS        = $07 ; 1
PRIM_GENLIST            = $08 ; 2
PRIM_ELEM               = $09 ; 2
PRIM_ANY                = $0A ; 2
PRIM_ALL                = $0B ; 2
PRIM_CONCATMAP          = $0C ; 2
PRIM_ATTRNAMES          = $0D ; 1
PRIM_ATTRVALUES         = $0E ; 1
PRIM_HASATTR            = $0F ; 2
PRIM_GETATTR            = $10 ; 2
PRIM_STRINGLENGTH       = $11 ; 1
PRIM_SUBSTRING          = $12 ; 3
PRIM_TOSTRING           = $13 ; 1
PRIM_CONCATSTRINGSSEP   = $14 ; 2
PRIM_THROW              = $15 ; 1
PRIM_ABORT              = $16 ; 1
PRIM_SEQ                = $17 ; 2
PRIM_DEEPSEQ            = $18 ; 2
PRIM_TYPEOF             = $19 ; 1
PRIM_ISINT              = $1A ; 1
PRIM_ISBOOL             = $1B ; 1
PRIM_ISSTRING           = $1C ; 1
PRIM_ISNULL             = $1D ; 1
PRIM_ISLIST             = $1E ; 1
PRIM_ISATTRS            = $1F ; 1
PRIM_ISFUNCTION         = $20 ; 1
PRIM_ADD                = $21 ; 2
PRIM_SUB                = $22 ; 2
PRIM_MUL                = $23 ; 2
PRIM_DIV                = $24 ; 2
PRIM_LESSTHAN           = $25 ; 2
PRIM_BITAND             = $26 ; 2
PRIM_BITOR              = $27 ; 2
PRIM_BITXOR             = $28 ; 2
PRIM_LISTTOATTRS        = $29 ; 1
PRIM_INTERSECTATTRS     = $2A ; 2
PRIM_REMOVEATTRS        = $2B ; 2
PRIM_PARTITION          = $2C ; 2
PRIM_BASENAMEOF         = $2D ; 1
PRIM_DIROF              = $2E ; 1
PRIM_TRYEVAL            = $2F ; 1
PRIM_COUNT              = $30

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
E_THROW         = $09
E_ABORT         = $0A
E_NOATTR        = $0B
E_INDEX         = $0C
E_NOVAR         = $0D
