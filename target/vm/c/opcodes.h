/* target/vm/c/opcodes.h -- the .nbc opcode table for the C VM.
 *
 * docs/BYTECODE.md is normative. This file, target/vm/opcodes.inc and
 * tools/nbc/src/opcodes.rs must match it exactly; checks.nbc-opcodes
 * (scripts/check-opcodes.py) parses all four and fails on any difference.
 * Keep the `#define OP_NAME 0xXX` form: the checker greps for it.
 */
#ifndef OPCODES_H
#define OPCODES_H

#define OP_CONST        0x01
#define OP_POP          0x02
#define OP_TRUE         0x03
#define OP_FALSE        0x04
#define OP_NULL         0x05

#define OP_ADD          0x10
#define OP_SUB          0x11
#define OP_MUL          0x12
#define OP_DIV          0x13
#define OP_NEG          0x14
#define OP_EQ           0x16
#define OP_LT           0x17
#define OP_LE           0x18
#define OP_GT           0x19
#define OP_GE           0x1A
#define OP_NOT          0x1B

#define OP_JMP          0x20
#define OP_JMP_IF_FALSE 0x21
#define OP_JMP_IF_TRUE  0x22
#define OP_ASSERT       0x23
#define OP_LIST         0x24
#define OP_CONCAT       0x25
#define OP_ATTRS        0x26
#define OP_SELECT       0x27
#define OP_TRY_SELECT   0x28
#define OP_JMP_IF_NOTFOUND 0x29
#define OP_HAS_ATTR     0x2A
#define OP_UPDATE       0x2B
#define OP_INTERPOLATE  0x2C

#define OP_ENV_NEW      0x30
#define OP_ENV_SET      0x31
#define OP_ENV_GET      0x32
#define OP_ENV_POP      0x33
#define OP_LAMBDA       0x34
#define OP_CALL         0x35
#define OP_WITH_PUSH    0x36
#define OP_WITH_GET     0x37
#define OP_TRY_WITH_GET 0x38
#define OP_TAIL_CALL    0x39
#define OP_ATTR_NAME    0x3A
#define OP_ATTRS_DYN    0x3B
#define OP_LAMBDA_ARGS  0x3C

#define OP_THUNK        0x40
#define OP_FORCE        0x41
#define OP_RETURN       0x42
#define OP_PRIM         0x43
#define OP_CLOSED_THUNK 0x44

/* Primops: PRIM_<NAME> id, then the arity in a comment. The Nix name is in
 * BYTECODE.md's table; the checker matches these by upper-casing the name and
 * dropping the prime. */
#define PRIM_LENGTH             0x00 /* 1 */
#define PRIM_HEAD               0x01 /* 1 */
#define PRIM_TAIL               0x02 /* 1 */
#define PRIM_ELEMAT             0x03 /* 2 */
#define PRIM_MAP                0x04 /* 2 */
#define PRIM_FILTER             0x05 /* 2 */
#define PRIM_FOLDL              0x06 /* 3 */
#define PRIM_CONCATLISTS        0x07 /* 1 */
#define PRIM_GENLIST            0x08 /* 2 */
#define PRIM_ELEM               0x09 /* 2 */
#define PRIM_ANY                0x0A /* 2 */
#define PRIM_ALL                0x0B /* 2 */
#define PRIM_CONCATMAP          0x0C /* 2 */
#define PRIM_ATTRNAMES          0x0D /* 1 */
#define PRIM_ATTRVALUES         0x0E /* 1 */
#define PRIM_HASATTR            0x0F /* 2 */
#define PRIM_GETATTR            0x10 /* 2 */
#define PRIM_STRINGLENGTH       0x11 /* 1 */
#define PRIM_SUBSTRING          0x12 /* 3 */
#define PRIM_TOSTRING           0x13 /* 1 */
#define PRIM_CONCATSTRINGSSEP   0x14 /* 2 */
#define PRIM_THROW              0x15 /* 1 */
#define PRIM_ABORT              0x16 /* 1 */
#define PRIM_SEQ                0x17 /* 2 */
#define PRIM_DEEPSEQ            0x18 /* 2 */
#define PRIM_TYPEOF             0x19 /* 1 */
#define PRIM_ISINT              0x1A /* 1 */
#define PRIM_ISBOOL             0x1B /* 1 */
#define PRIM_ISSTRING           0x1C /* 1 */
#define PRIM_ISNULL             0x1D /* 1 */
#define PRIM_ISLIST             0x1E /* 1 */
#define PRIM_ISATTRS            0x1F /* 1 */
#define PRIM_ISFUNCTION         0x20 /* 1 */
#define PRIM_ADD                0x21 /* 2 */
#define PRIM_SUB                0x22 /* 2 */
#define PRIM_MUL                0x23 /* 2 */
#define PRIM_DIV                0x24 /* 2 */
#define PRIM_LESSTHAN           0x25 /* 2 */
#define PRIM_BITAND             0x26 /* 2 */
#define PRIM_BITOR              0x27 /* 2 */
#define PRIM_BITXOR             0x28 /* 2 */
#define PRIM_LISTTOATTRS        0x29 /* 1 */
#define PRIM_INTERSECTATTRS     0x2A /* 2 */
#define PRIM_REMOVEATTRS        0x2B /* 2 */
#define PRIM_PARTITION          0x2C /* 2 */
#define PRIM_BASENAMEOF         0x2D /* 1 */
#define PRIM_DIROF              0x2E /* 1 */
#define PRIM_TRYEVAL            0x2F /* 1 */
#define PRIM_HASHSTRING         0x30 /* 2 */
#define PRIM_DERIVATIONSTRICT   0x31 /* 1 */
#define PRIM_HASCONTEXT         0x32 /* 1 */
#define PRIM_UNSAFEDISCARDSTRINGCONTEXT 0x33 /* 1 */
#define PRIM_UNSAFEDISCARDOUTPUTDEPENDENCY 0x34 /* 1 */
#define PRIM_ADDDRVOUTPUTDEPENDENCIES 0x35 /* 1 */
#define PRIM_GETCONTEXT         0x36 /* 1 */
#define PRIM_APPENDCONTEXT      0x37 /* 2 */
#define PRIM_TOFILE             0x38 /* 2 */
#define PRIM_ISFLOAT            0x39 /* 1 */
#define PRIM_FLOOR              0x3A /* 1 */
#define PRIM_CEIL               0x3B /* 1 */
#define PRIM_MAPATTRS           0x3C /* 2 */
#define PRIM_CATATTRS           0x3D /* 2 */
#define PRIM_GROUPBY            0x3E /* 2 */
#define PRIM_ZIPATTRSWITH       0x3F /* 2 */
#define PRIM_ISPATH             0x40 /* 1 */
#define PRIM_GENERICCLOSURE     0x41 /* 1 */
#define PRIM_SORT               0x42 /* 2 */
#define PRIM_COMPAREVERSIONS    0x43 /* 2 */
#define PRIM_SPLITVERSION       0x44 /* 1 */
#define PRIM_PARSEDRVNAME       0x45 /* 1 */
#define PRIM_REPLACESTRINGS     0x46 /* 3 */
#define PRIM_FUNCTIONARGS       0x47 /* 1 */
#define PRIM_TOJSON             0x48 /* 1 */
#define PRIM_FROMJSON           0x49 /* 1 */
#define PRIM_TOXML              0x4A /* 1 */
#define PRIM_TOPATH             0x4B /* 1 */
#define PRIM_MATCH              0x4C /* 2 */
#define PRIM_SPLIT              0x4D /* 2 */
/* $4E-$5C: the builtins that need GS/OS (D26): each is E_GSOS, by name */
#define PRIM_FETCHGIT             0x4E /* 1 */
#define PRIM_FETCHMERCURIAL       0x4F /* 1 */
#define PRIM_FETCHTARBALL         0x50 /* 1 */
#define PRIM_FETCHTREE            0x51 /* 1 */
#define PRIM_FETCHURL             0x52 /* 1 */
#define PRIM_FILTERSOURCE         0x53 /* 2 */
#define PRIM_GETENV               0x54 /* 1 */
#define PRIM_GETFLAKE             0x55 /* 1 */
#define PRIM_HASHFILE             0x56 /* 2 */
#define PRIM_PATH                 0x57 /* 1 */
#define PRIM_PATHEXISTS           0x58 /* 1 */
#define PRIM_READDIR              0x59 /* 1 */
#define PRIM_READFILE             0x5A /* 1 */
#define PRIM_READFILETYPE         0x5B /* 1 */
#define PRIM_STOREPATH            0x5C /* 1 */
#define PRIM_FINDFILE             0x5D /* 2 */
#define PRIM_IMPORT               0x5E /* 1 */
#define PRIM_SCOPEDIMPORT         0x5F /* 2 */
#define PRIM_COUNT              0x32

/* Constant kinds are heap tags (docs/HEAP.md). */
#define K_INT           0x01
#define K_STRING        0x04
#define K_PATH          0x05
#define K_FLOAT         0x0D

/* Error codes reported at $0321 when the status byte is $FF. */
#define E_BADOP         0x01
#define E_TYPE          0x02
#define E_DIVZERO       0x03
#define E_INFREC        0x04
#define E_NOMEM         0x05
#define E_BADNBC        0x06
#define E_NOCARD        0x07
#define E_ASSERT        0x08
#define E_THROW         0x09
#define E_ABORT         0x0A
#define E_NOATTR        0x0B
#define E_INDEX         0x0C
#define E_NOVAR         0x0D
#define E_UNSUP         0x0E    /* valid Nix the VM does not implement yet (DERIVATION.md) */
#define E_OVERFLOW      0x0F    /* + - * / overflowed: an error in Nix 2.34, not a wrap */
#define E_DUPATTR       0x10    /* a computed attribute name already in its set */
#define E_REGEX         0x11    /* not a regular expression (match, split): Nix's "invalid regular expression" */
#define E_GSOS          0x12    /* a builtin that needs GS/OS -- files, the environment, the network (D26) */

#endif
