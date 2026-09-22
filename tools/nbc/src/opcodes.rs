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

pub const OP_ENV_NEW: u8 = 0x30;
pub const OP_ENV_SET: u8 = 0x31;
pub const OP_ENV_GET: u8 = 0x32;
pub const OP_ENV_POP: u8 = 0x33;
pub const OP_LAMBDA: u8 = 0x34;
pub const OP_CALL: u8 = 0x35;

pub const OP_THUNK: u8 = 0x40;
pub const OP_FORCE: u8 = 0x41;
pub const OP_RETURN: u8 = 0x42;

/// Constant kinds are heap tags (docs/HEAP.md).
pub const K_INT: u8 = 0x01;
#[allow(dead_code)] // slice 2
pub const K_STRING: u8 = 0x04;

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
        OP_ENV_NEW => ("ENV_NEW", 2),
        OP_ENV_SET => ("ENV_SET", 2),
        OP_ENV_GET => ("ENV_GET", 4),
        OP_ENV_POP => ("ENV_POP", 0),
        OP_LAMBDA => ("LAMBDA", 2),
        OP_CALL => ("CALL", 0),
        OP_THUNK => ("THUNK", 2),
        OP_FORCE => ("FORCE", 0),
        OP_RETURN => ("RETURN", 0),
        _ => return None,
    })
}
