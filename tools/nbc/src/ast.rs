//! The generator's own AST: exactly the slice the VM can run, nothing more.
//!
//! rnix parses all of Nix; `front.rs` lowers its tree into this enum and refuses,
//! with a named reason, every construct outside the current slice. Keeping the
//! compiler behind a small AST means widening the slice is a matter of adding
//! variants here and cases in `compiler.rs` -- the parser never changes again.

use std::fmt;

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Pos {
    pub line: usize,
    pub col: usize,
}

impl fmt::Display for Pos {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}:{}", self.line, self.col)
    }
}

/// The generator's only error type. Everything it cannot handle -- a parse
/// error, a feature outside the current slice, an unbound name -- is
/// "unsupported". The scoreboard counts these separately from VM failures and
/// histograms `what`, which is how the next slice gets chosen.
#[derive(Debug, Clone, PartialEq)]
pub struct Unsupported {
    pub pos: Pos,
    pub what: String,
}

impl fmt::Display for Unsupported {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}: unsupported: {}", self.pos, self.what)
    }
}

pub fn unsupported<T>(pos: Pos, what: impl Into<String>) -> Result<T, Unsupported> {
    Err(Unsupported { pos, what: what.into() })
}

#[derive(Debug, Clone, PartialEq)]
pub enum BinOp {
    Add,
    Sub,
    Mul,
    Div,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
}

#[derive(Debug, Clone, PartialEq)]
pub enum Expr {
    Int(i64),
    /// An identifier. `true`, `false` and `null` are identifiers in Nix, resolved
    /// by the compiler against the scope chain so that shadowing them works.
    Var(String, Pos),
    Neg(Box<Expr>),
    Not(Box<Expr>),
    Bin(BinOp, Box<Expr>, Box<Expr>),
    And(Box<Expr>, Box<Expr>),
    Or(Box<Expr>, Box<Expr>),
    Implies(Box<Expr>, Box<Expr>),
    If(Box<Expr>, Box<Expr>, Box<Expr>),
    Let(Vec<(String, Pos, Expr)>, Box<Expr>),
    /// `param: body`. Formals are desugared by the compiler (BYTECODE.md
    /// "Desugarings").
    Lambda(Param, Pos, Box<Expr>),
    /// `with e; body`.
    With(Box<Expr>, Box<Expr>),
    /// `f arg`.
    Apply(Box<Expr>, Box<Expr>),
    /// `assert cond; body`.
    Assert(Box<Expr>, Box<Expr>),
    /// A string: literal parts and `${…}` interpolations, escapes and indentation
    /// already resolved by rnix. A plain literal is a single `Lit` part.
    Str(Vec<StrPart>),
    List(Vec<Expr>),
    /// Static keys only, unsorted here; the compiler sorts by bytes.
    AttrSet(Vec<(String, Pos, Expr)>),
    /// `e.a.b`, optionally `or default` — the default applies if any step is
    /// missing or not a set.
    Select(Box<Expr>, Vec<String>, Option<Box<Expr>>),
    /// `e ? a.b`.
    HasAttr(Box<Expr>, Vec<String>),
    /// `a // b`.
    Update(Box<Expr>, Box<Expr>),
    /// `a ++ b`.
    Concat(Box<Expr>, Box<Expr>),
    /// `inherit x;` as a let binding: `x` from the scope OUTSIDE the let.
    Inherit(String, Pos),
    /// Compiler-internal: evaluate `a` for effect (force it), then `b`.
    /// Formals force their argument set on entry, as Nix does.
    Seq(Box<Expr>, Box<Expr>),
    /// `import ./file.nix`: the file's canonical path, and its lowered body.
    /// It compiles to its own chunk and a CLOSED_THUNK over it, because an
    /// imported file is evaluated in its OWN scope and this IR resolves names
    /// against whatever encloses them. The path is carried so the compiler can
    /// give one file ONE chunk however many sites import it, which is what lets
    /// the VM share a single thunk (docs/BYTECODE.md "Imports").
    Import(String, Box<Expr>),
}

#[derive(Debug, Clone, PartialEq)]
pub enum StrPart {
    Lit(String),
    Interp(Expr),
}

#[derive(Debug, Clone, PartialEq)]
pub enum Param {
    Ident(String),
    /// `{ a, b ? default, ... } @ bind`
    Formals { entries: Vec<(String, Pos, Option<Expr>)>, ellipsis: bool, bind: Option<String> },
}
