//! The opcode table. docs/BYTECODE.md is normative; this file and
//! target/vm/opcodes.inc must match it exactly, and checks.nbc-opcodes asserts
//! that they do (scripts/check-opcodes.py parses all three).
//!
//! Keep the `pub const OP_NAME: u8 = 0x..;` form: the checker greps for it.

pub const OP_CONST: u8 = 0x01;
pub const OP_POP: u8 = 0x02;
pub const OP_TRUE: u8 = 0x03;
pub const OP_FALSE: u8 = 0x04;
pub const OP_NULL: u8 = 0x05;

pub const OP_ADD: u8 = 0x10;
pub const OP_SUB: u8 = 0x11;
pub const OP_MUL: u8 = 0x12;
pub const OP_DIV: u8 = 0x13;
pub const OP_NEG: u8 = 0x14;
pub const OP_EQ: u8 = 0x16;
pub const OP_LT: u8 = 0x17;
pub const OP_LE: u8 = 0x18;
pub const OP_GT: u8 = 0x19;
pub const OP_GE: u8 = 0x1A;
pub const OP_NOT: u8 = 0x1B;

pub const OP_JMP: u8 = 0x20;
pub const OP_JMP_IF_FALSE: u8 = 0x21;
pub const OP_JMP_IF_TRUE: u8 = 0x22;
pub const OP_ASSERT: u8 = 0x23;
pub const OP_LIST: u8 = 0x24;
pub const OP_CONCAT: u8 = 0x25;
pub const OP_ATTRS: u8 = 0x26;
pub const OP_SELECT: u8 = 0x27;
pub const OP_TRY_SELECT: u8 = 0x28;
pub const OP_JMP_IF_NOTFOUND: u8 = 0x29;
pub const OP_HAS_ATTR: u8 = 0x2A;
pub const OP_UPDATE: u8 = 0x2B;
pub const OP_INTERPOLATE: u8 = 0x2C;

pub const OP_ENV_NEW: u8 = 0x30;
pub const OP_ENV_SET: u8 = 0x31;
pub const OP_ENV_GET: u8 = 0x32;
pub const OP_ENV_POP: u8 = 0x33;
pub const OP_LAMBDA: u8 = 0x34;
pub const OP_CALL: u8 = 0x35;
pub const OP_WITH_PUSH: u8 = 0x36;
pub const OP_WITH_GET: u8 = 0x37;
pub const OP_TRY_WITH_GET: u8 = 0x38;
pub const OP_TAIL_CALL: u8 = 0x39;
pub const OP_ATTR_NAME: u8 = 0x3A;
pub const OP_ATTRS_DYN: u8 = 0x3B;

pub const OP_THUNK: u8 = 0x40;
pub const OP_FORCE: u8 = 0x41;
pub const OP_RETURN: u8 = 0x42;
pub const OP_PRIM: u8 = 0x43;
/// A thunk that captures the EMPTY environment, for `import`: an imported file
/// is evaluated in its own scope, and the IR's names are symbolic, so an
/// ordinary THUNK would let it capture the importer's bindings.
pub const OP_CLOSED_THUNK: u8 = 0x44;

/// Constant kinds are heap tags (docs/HEAP.md).
pub const K_INT: u8 = 0x01;
pub const K_STRING: u8 = 0x04;
pub const K_PATH: u8 = 0x05;
pub const K_FLOAT: u8 = 0x0D;

/// Primops: (Nix name, id, arity). BYTECODE.md "Primops" is normative and
/// target/vm/opcodes.inc must agree; checks.nbc-opcodes diffs all three.
/// The Nix name is what `builtins.NAME` resolves against.
pub const PRIMOPS: &[(&str, u8, u8)] = &[
    ("length", 0x00, 1),
    ("head", 0x01, 1),
    ("tail", 0x02, 1),
    ("elemAt", 0x03, 2),
    ("map", 0x04, 2),
    ("filter", 0x05, 2),
    ("foldl'", 0x06, 3),
    ("concatLists", 0x07, 1),
    ("genList", 0x08, 2),
    ("elem", 0x09, 2),
    ("any", 0x0A, 2),
    ("all", 0x0B, 2),
    ("concatMap", 0x0C, 2),
    ("attrNames", 0x0D, 1),
    ("attrValues", 0x0E, 1),
    ("hasAttr", 0x0F, 2),
    ("getAttr", 0x10, 2),
    ("stringLength", 0x11, 1),
    ("substring", 0x12, 3),
    ("toString", 0x13, 1),
    ("concatStringsSep", 0x14, 2),
    ("throw", 0x15, 1),
    ("abort", 0x16, 1),
    ("seq", 0x17, 2),
    ("deepSeq", 0x18, 2),
    ("typeOf", 0x19, 1),
    ("isInt", 0x1A, 1),
    ("isBool", 0x1B, 1),
    ("isString", 0x1C, 1),
    ("isNull", 0x1D, 1),
    ("isList", 0x1E, 1),
    ("isAttrs", 0x1F, 1),
    ("isFunction", 0x20, 1),
    ("add", 0x21, 2),
    ("sub", 0x22, 2),
    ("mul", 0x23, 2),
    ("div", 0x24, 2),
    ("lessThan", 0x25, 2),
    ("bitAnd", 0x26, 2),
    ("bitOr", 0x27, 2),
    ("bitXor", 0x28, 2),
    ("listToAttrs", 0x29, 1),
    ("intersectAttrs", 0x2A, 2),
    ("removeAttrs", 0x2B, 2),
    ("partition", 0x2C, 2),
    ("baseNameOf", 0x2D, 1),
    ("dirOf", 0x2E, 1),
    ("tryEval", 0x2F, 1),
    ("hashString", 0x30, 2),
    ("derivationStrict", 0x31, 1),
    ("hasContext", 0x32, 1),
    ("unsafeDiscardStringContext", 0x33, 1),
    ("unsafeDiscardOutputDependency", 0x34, 1),
    ("addDrvOutputDependencies", 0x35, 1),
    ("getContext", 0x36, 1),
    ("appendContext", 0x37, 2),
    ("toFile", 0x38, 2),
    ("isFloat", 0x39, 1),
    ("floor", 0x3A, 1),
    ("ceil", 0x3B, 1),
    ("mapAttrs", 0x3C, 2),
    ("catAttrs", 0x3D, 2),
    ("groupBy", 0x3E, 2),
    ("zipAttrsWith", 0x3F, 2),
    ("isPath", 0x40, 1),
    ("genericClosure", 0x41, 1),
];

/// The builtins Nix also exposes unqualified.
pub const GLOBALS: &[&str] = &[
    "map", "toString", "throw", "abort", "isNull", "removeAttrs", "baseNameOf", "dirOf",
    "derivation", "derivationStrict",
];

/// Builtins that are values rather than functions: a string constant each,
/// emitted where `builtins.NAME` is and included in `builtins` as a set.
/// `currentSystem` is what an IIgs reports (DECISIONS.md D23).
pub const BUILTIN_VALUES: &[(&str, &str)] = &[("currentSystem", "w65-gsos")];

pub fn builtin_value(name: &str) -> Option<&'static str> {
    BUILTIN_VALUES.iter().find(|(n, _)| *n == name).map(|(_, v)| *v)
}

pub fn primop(name: &str) -> Option<(u8, u8)> {
    PRIMOPS.iter().find(|(n, _, _)| *n == name).map(|(_, id, arity)| (*id, *arity))
}

pub fn primop_name(id: u8) -> Option<&'static str> {
    PRIMOPS.iter().find(|(_, i, _)| *i == id).map(|(n, _, _)| *n)
}

/// Name and operand width in bytes, for the disassembler.
pub fn describe(op: u8) -> Option<(&'static str, usize)> {
    Some(match op {
        OP_CONST => ("CONST", 2),
        OP_POP => ("POP", 0),
        OP_TRUE => ("TRUE", 0),
        OP_FALSE => ("FALSE", 0),
        OP_NULL => ("NULL", 0),
        OP_ADD => ("ADD", 0),
        OP_SUB => ("SUB", 0),
        OP_MUL => ("MUL", 0),
        OP_DIV => ("DIV", 0),
        OP_NEG => ("NEG", 0),
        OP_EQ => ("EQ", 0),
        OP_LT => ("LT", 0),
        OP_LE => ("LE", 0),
        OP_GT => ("GT", 0),
        OP_GE => ("GE", 0),
        OP_NOT => ("NOT", 0),
        OP_JMP => ("JMP", 2),
        OP_JMP_IF_FALSE => ("JMP_IF_FALSE", 2),
        OP_JMP_IF_TRUE => ("JMP_IF_TRUE", 2),
        OP_ASSERT => ("ASSERT", 0),
        OP_LIST => ("LIST", 2),
        OP_CONCAT => ("CONCAT", 0),
        OP_ATTRS => ("ATTRS", 2),
        OP_SELECT => ("SELECT", 0),
        OP_TRY_SELECT => ("TRY_SELECT", 0),
        OP_JMP_IF_NOTFOUND => ("JMP_IF_NOTFOUND", 2),
        OP_HAS_ATTR => ("HAS_ATTR", 0),
        OP_UPDATE => ("UPDATE", 0),
        OP_INTERPOLATE => ("INTERPOLATE", 2),
        OP_PRIM => ("PRIM", 1),
        OP_ENV_NEW => ("ENV_NEW", 2),
        OP_ENV_SET => ("ENV_SET", 2),
        OP_ENV_GET => ("ENV_GET", 4),
        OP_ENV_POP => ("ENV_POP", 0),
        OP_LAMBDA => ("LAMBDA", 2),
        OP_CALL => ("CALL", 0),
        OP_WITH_PUSH => ("WITH_PUSH", 0),
        OP_WITH_GET => ("WITH_GET", 0),
        OP_TRY_WITH_GET => ("TRY_WITH_GET", 0),
        OP_TAIL_CALL => ("TAIL_CALL", 0),
        OP_ATTR_NAME => ("ATTR_NAME", 0),
        OP_ATTRS_DYN => ("ATTRS_DYN", 4),
        OP_THUNK => ("THUNK", 2),
        OP_FORCE => ("FORCE", 0),
        OP_RETURN => ("RETURN", 0),
        _ => return None,
    })
}
