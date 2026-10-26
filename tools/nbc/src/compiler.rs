//! AST -> chunks, following the thunk-placement rules in docs/BYTECODE.md.
//!
//! Scopes mirror the VM's environment chain exactly: each `let` (and each lambda
//! parameter) pushes one scope, and a variable resolves to (depth, index) where
//! depth counts enclosing scopes and index is 1-based within the scope (element 0
//! of an environment is its parent).

use std::collections::HashMap;

use crate::ast::{unsupported, BinOp, Expr, Param, Pos, StrPart, Unsupported};
use crate::opcodes::*;

#[derive(Debug, Clone, PartialEq)]
pub enum Const {
    Int(i64),
    /// IEEE-754 binary64 bits (docs/FLOAT.md).
    Float(u64),
    Str(String),
    /// An absolute, canonical path (front.rs canon_path).
    Path(String),
}

#[derive(Debug, Default, Clone, PartialEq)]
pub struct Chunk {
    pub consts: Vec<Const>,
    pub code: Vec<u8>,
}

/// One environment object at runtime: a let's bindings, a lambda's parameter,
/// or a `with` (no names; identifiers fall through to it at runtime).
struct Scope {
    names: Vec<String>,
    with: bool,
}

pub struct Compiler {
    pub chunks: Vec<Chunk>,
    scopes: Vec<Scope>,
    /// canonical path -> the chunk its body was compiled into, so one file is
    /// one chunk however many sites import it.
    imported: std::collections::HashMap<String, usize>,
    /// Synthesised primop lambdas, one per program: Nix name -> chunk index.
    builtins: HashMap<String, usize>,
    /// Position of the last CALL emitted into each chunk, for the tail-call
    /// peephole in `emit_return`.
    last_call: HashMap<usize, usize>,
}

/// What a name resolved to.
enum Binding {
    /// (depth, index) into the environment chain.
    Env(u16, u16),
    /// The global `true`, `false`, `null`.
    Literal(u8),
    /// An unqualified global primop such as `map` or `toString`.
    Global(&'static str),
    /// Not lexically bound, but a `with` is in scope: looked up at runtime, with
    /// the named global as the fallback if there is one.
    With(Option<&'static str>),
    /// `builtins` itself, used as a value rather than as `builtins.NAME`: the
    /// whole set, one attribute per primop the VM has.
    BuiltinsSet,
}

pub fn compile(program: &Expr) -> Result<Vec<Chunk>, Unsupported> {
    let mut c = Compiler {
        chunks: vec![Chunk::default()],
        scopes: Vec::new(),
        imported: std::collections::HashMap::new(),
        builtins: HashMap::new(),
        last_call: HashMap::new(),
    };
    c.strict(program, 0)?;
    c.emit_return(0);
    Ok(c.chunks)
}

/// The name the desugared formals bind the whole argument set to when the
/// source gives none. Not a valid Nix identifier, so it cannot collide.
const FORMALS_ARG: &str = "@args";

fn nowhere() -> Pos {
    Pos { line: 0, col: 0 }
}

impl Compiler {
    fn emit(&mut self, chunk: usize, b: u8) {
        self.chunks[chunk].code.push(b);
    }
    /// RETURN, turning a CALL immediately before it into a TAIL_CALL: nothing
    /// can land between the two, so that call is in tail position.
    fn emit_return(&mut self, chunk: usize) {
        let len = self.chunks[chunk].code.len();
        if len > 0 && self.last_call.get(&chunk) == Some(&(len - 1)) {
            self.chunks[chunk].code[len - 1] = OP_TAIL_CALL;
        }
        self.emit(chunk, OP_RETURN);
    }
    fn emit_u16(&mut self, chunk: usize, v: u16) {
        self.chunks[chunk].code.extend_from_slice(&v.to_le_bytes());
    }
    /// Emits a jump with a placeholder offset; returns the position to patch.
    fn emit_jump(&mut self, chunk: usize, op: u8) -> usize {
        self.emit(chunk, op);
        let at = self.chunks[chunk].code.len();
        self.emit_u16(chunk, 0);
        at
    }
    /// Patches the jump at `at` to land at the current end of the chunk. Offsets are
    /// relative to the end of the jump instruction.
    fn patch_jump(&mut self, chunk: usize, at: usize) -> Result<(), Unsupported> {
        let target = self.chunks[chunk].code.len();
        let rel = target as i64 - (at as i64 + 2);
        let rel16 = i16::try_from(rel)
            .map_err(|_| Unsupported { pos: nowhere(), what: "jump offset out of range".into() })?;
        self.chunks[chunk].code[at..at + 2].copy_from_slice(&rel16.to_le_bytes());
        Ok(())
    }
    fn constant(&mut self, chunk: usize, k: Const) -> Result<u16, Unsupported> {
        let consts = &mut self.chunks[chunk].consts;
        let idx = match consts.iter().position(|c| *c == k) {
            Some(i) => i,
            None => {
                consts.push(k);
                consts.len() - 1
            }
        };
        u16::try_from(idx)
            .map_err(|_| Unsupported { pos: nowhere(), what: "more than 65535 constants in one chunk".into() })
    }
    fn emit_const(&mut self, chunk: usize, k: Const) -> Result<(), Unsupported> {
        let idx = self.constant(chunk, k)?;
        self.emit(chunk, OP_CONST);
        self.emit_u16(chunk, idx);
        Ok(())
    }
    fn count(n: usize, what: &str) -> Result<u16, Unsupported> {
        u16::try_from(n).map_err(|_| Unsupported { pos: nowhere(), what: format!("more than 65535 {what}") })
    }

    /// Resolves `name`, ignoring the innermost `skip` scopes. Lexical bindings
    /// first, then the literals, then -- if a `with` is in scope -- a runtime
    /// lookup with the global of that name (if any) as the fallback, then the
    /// globals themselves.
    fn resolve_from(&self, name: &str, skip: usize, pos: Pos) -> Result<Binding, Unsupported> {
        for (depth, scope) in self.scopes.iter().rev().enumerate().skip(skip) {
            if let Some(i) = scope.names.iter().position(|n| n == name) {
                return Ok(Binding::Env(depth as u16, (i + 1) as u16));
            }
        }
        match name {
            "true" => return Ok(Binding::Literal(OP_TRUE)),
            "false" => return Ok(Binding::Literal(OP_FALSE)),
            "null" => return Ok(Binding::Literal(OP_NULL)),
            _ => {}
        }
        let global = GLOBALS.iter().find(|g| **g == name).copied();
        if self.scopes.iter().rev().skip(skip).any(|s| s.with) {
            return Ok(Binding::With(global));
        }
        match global {
            Some(g) => Ok(Binding::Global(g)),
            // `builtins` is not in GLOBALS -- that list is the names Nix also
            // exposes unqualified -- so an unshadowed `builtins` reaching here
            // is the set itself: `with builtins;`, `builtins ? foo`, passing it
            // to a function.
            None if name == "builtins" => Ok(Binding::BuiltinsSet),
            None => unsupported(pos, format!("unbound variable `{name}`")),
        }
    }
    fn resolve(&self, name: &str, pos: Pos) -> Result<Binding, Unsupported> {
        self.resolve_from(name, 0, pos)
    }
    /// Could `name` mean something other than the global? True for a `with`,
    /// whose set is not known here.
    fn is_shadowed(&self, name: &str) -> bool {
        self.scopes.iter().any(|s| s.with || s.names.iter().any(|n| n == name))
    }
    /// Is `name` bound by a `let`, a lambda parameter or a `rec`? Unlike
    /// `is_shadowed`, a `with` does not count -- a with-set MIGHT define the
    /// name, which is a run-time question, not this one.
    fn is_lexically_bound(&self, name: &str) -> bool {
        self.scopes.iter().any(|s| s.names.iter().any(|n| n == name))
    }

    /// A runtime `with` lookup of `name`, leaving the value unforced on the
    /// stack; `global` is what a miss falls back to.
    fn emit_with_get(&mut self, name: &str, global: Option<&'static str>, pos: Pos, chunk: usize) -> Result<(), Unsupported> {
        self.emit_const(chunk, Const::Str(name.to_string()))?;
        // A `with` set may or may not define the name, so anything that has a
        // fallback is TRY_WITH_GET plus the fallback on a miss. `builtins` has
        // one even though it is not in GLOBALS: it lives in Nix's base
        // environment, so `with s; builtins` is the set unless `s` defines it.
        if global.is_none() && name != "builtins" {
            self.emit(chunk, OP_WITH_GET);
            return Ok(());
        }
        self.emit(chunk, OP_TRY_WITH_GET);
        let miss = self.emit_jump(chunk, OP_JMP_IF_NOTFOUND);
        let end = self.emit_jump(chunk, OP_JMP);
        self.patch_jump(chunk, miss)?;
        self.emit(chunk, OP_POP);
        match global {
            Some(g) => self.emit_builtin(g, pos, chunk)?,
            None => self.emit_builtins_set(pos, chunk)?,
        }
        self.patch_jump(chunk, end)?;
        Ok(())
    }

    /// `builtins` as a value: an attribute set with one entry per primop, whose
    /// values are the same synthesised lambdas `builtins.NAME` would give. Nix
    /// keeps attributes sorted by name bytes and the VM relies on it, so the
    /// names are sorted here rather than taken in table order.
    ///
    /// This is not cheap -- 47 attributes, a name constant each, and 73 chunks
    /// of curried lambdas -- but `with builtins;` is once per program, which is
    /// how the corpus uses it. Every primop is eagerly built, because the set is
    /// a value: something may iterate it with `attrNames`.
    fn emit_builtins_set(&mut self, pos: Pos, chunk: usize) -> Result<(), Unsupported> {
        let mut names: Vec<&'static str> = PRIMOPS.iter().map(|(n, _, _)| *n)
            .chain(BUILTIN_VALUES.iter().map(|(n, _)| *n)).collect();
        names.sort_by(|a, b| a.as_bytes().cmp(b.as_bytes()));
        for name in &names {
            self.emit_const(chunk, Const::Str((*name).to_string()))?;
            self.emit_builtin(name, pos, chunk)?;
        }
        let n = Self::count(names.len(), "attributes")?;
        self.emit(chunk, OP_ATTRS);
        self.emit_u16(chunk, n);
        Ok(())
    }

    /// `builtins.NAME` with `builtins` unshadowed, or a global: the primop's
    /// synthesised lambda. Refused by name if the VM lacks the primop, so the
    /// scoreboard can say which one would move the most tests.
    fn emit_builtin(&mut self, name: &str, pos: Pos, chunk: usize) -> Result<(), Unsupported> {
        if name == "derivation" {
            return self.emit_derivation(pos, chunk);
        }
        if let Some(v) = builtin_value(name) {
            return self.emit_const(chunk, Const::Str(v.to_string()));
        }
        let Some((id, arity)) = primop(name) else {
            return unsupported(pos, format!("builtins.{name}"));
        };
        let entry = match self.builtins.get(name) {
            Some(&c) => c,
            None => {
                // One lambda per argument, nested; the innermost pushes every
                // argument outermost-first and runs the primop.
                let mut inner = self.chunks.len();
                self.chunks.push(Chunk::default());
                for d in (0..arity).rev() {
                    self.emit(inner, OP_ENV_GET);
                    self.emit_u16(inner, d as u16);
                    self.emit_u16(inner, 1);
                }
                self.emit(inner, OP_PRIM);
                self.emit(inner, id);
                self.emit_return(inner);
                for _ in 1..arity {
                    let outer = self.chunks.len();
                    self.chunks.push(Chunk::default());
                    self.emit(outer, OP_LAMBDA);
                    self.emit_u16(outer, inner as u16);
                    self.emit_return(outer);
                    inner = outer;
                }
                self.builtins.insert(name.to_string(), inner);
                inner
            }
        };
        self.emit(chunk, OP_LAMBDA);
        self.emit_u16(chunk, entry as u16);
        Ok(())
    }

    /// `derivation` is not a primop: in Nix it is derivation.nix wrapped around
    /// the `derivationStrict` primop, and so it is here -- that file, embedded,
    /// compiled as an import and forced (docs/DERIVATION.md). One chunk however
    /// many sites use it, so every site gets the same function. Not in the
    /// `builtins` set value, which would compile it into every `with builtins;`
    /// program; a `with builtins;` lookup misses and falls back to this.
    fn emit_derivation(&mut self, pos: Pos, chunk: usize) -> Result<(), Unsupported> {
        let e = match crate::front::parse(include_str!("derivation.nix")) {
            Ok(e) => e,
            Err(u) => return unsupported(pos, format!("derivation.nix: {}", u.what)),
        };
        self.closed_thunk("<nix/derivation-internal.nix>", &e, chunk)?;
        self.emit(chunk, OP_FORCE);
        Ok(())
    }

    /// Compiles `e` so that its VALUE is on top of the stack afterwards.
    fn strict(&mut self, e: &Expr, chunk: usize) -> Result<(), Unsupported> {
        match e {
            Expr::Int(v) => self.emit_const(chunk, Const::Int(*v))?,
            Expr::Float(v) => self.emit_const(chunk, Const::Float(*v))?,
            Expr::Path(p) => self.emit_const(chunk, Const::Path(p.clone()))?,
            Expr::Var(name, pos) => match self.resolve(name, *pos)? {
                Binding::Literal(op) => self.emit(chunk, op),
                Binding::Global(g) => self.emit_builtin(g, *pos, chunk)?,
                Binding::BuiltinsSet => self.emit_builtins_set(*pos, chunk)?,
                Binding::Env(d, i) => {
                    self.emit(chunk, OP_ENV_GET);
                    self.emit_u16(chunk, d);
                    self.emit_u16(chunk, i);
                    self.emit(chunk, OP_FORCE);
                }
                Binding::With(g) => {
                    self.emit_with_get(name, g, *pos, chunk)?;
                    self.emit(chunk, OP_FORCE);
                }
            },
            Expr::Inherit(name, pos) => match self.resolve_from(name, 1, *pos)? {
                Binding::Literal(op) => self.emit(chunk, op),
                Binding::Global(g) => self.emit_builtin(g, *pos, chunk)?,
                Binding::BuiltinsSet => self.emit_builtins_set(*pos, chunk)?,
                Binding::Env(d, i) => {
                    self.emit(chunk, OP_ENV_GET);
                    self.emit_u16(chunk, d);
                    self.emit_u16(chunk, i);
                    self.emit(chunk, OP_FORCE);
                }
                Binding::With(g) => {
                    self.emit_with_get(name, g, *pos, chunk)?;
                    self.emit(chunk, OP_FORCE);
                }
            },
            Expr::With(set, body) => {
                // The set is forced only when an identifier needs it: `with
                // (abort "x"); 42` is 42.
                self.lazy(set, chunk)?;
                self.emit(chunk, OP_WITH_PUSH);
                self.scopes.push(Scope { names: Vec::new(), with: true });
                self.strict(body, chunk)?;
                self.emit(chunk, OP_ENV_POP);
                self.scopes.pop();
            }
            Expr::Neg(a) => {
                self.strict(a, chunk)?;
                self.emit(chunk, OP_NEG);
            }
            Expr::Not(a) => {
                self.strict(a, chunk)?;
                self.emit(chunk, OP_NOT);
            }
            Expr::Bin(op, a, b) => {
                self.strict(a, chunk)?;
                self.strict(b, chunk)?;
                match op {
                    BinOp::Add => self.emit(chunk, OP_ADD),
                    BinOp::Sub => self.emit(chunk, OP_SUB),
                    BinOp::Mul => self.emit(chunk, OP_MUL),
                    BinOp::Div => self.emit(chunk, OP_DIV),
                    BinOp::Eq => self.emit(chunk, OP_EQ),
                    BinOp::Ne => {
                        self.emit(chunk, OP_EQ);
                        self.emit(chunk, OP_NOT);
                    }
                    BinOp::Lt => self.emit(chunk, OP_LT),
                    BinOp::Le => self.emit(chunk, OP_LE),
                    BinOp::Gt => self.emit(chunk, OP_GT),
                    BinOp::Ge => self.emit(chunk, OP_GE),
                }
            }
            Expr::And(a, b) => {
                self.strict(a, chunk)?;
                let j = self.emit_jump(chunk, OP_JMP_IF_FALSE);
                self.emit(chunk, OP_POP);
                self.strict(b, chunk)?;
                self.patch_jump(chunk, j)?;
            }
            Expr::Or(a, b) => {
                self.strict(a, chunk)?;
                let j = self.emit_jump(chunk, OP_JMP_IF_TRUE);
                self.emit(chunk, OP_POP);
                self.strict(b, chunk)?;
                self.patch_jump(chunk, j)?;
            }
            Expr::Implies(a, b) => {
                // a -> b  ==  !a || b
                self.strict(a, chunk)?;
                self.emit(chunk, OP_NOT);
                let j = self.emit_jump(chunk, OP_JMP_IF_TRUE);
                self.emit(chunk, OP_POP);
                self.strict(b, chunk)?;
                self.patch_jump(chunk, j)?;
            }
            Expr::If(c, a, b) => {
                self.strict(c, chunk)?;
                let to_else = self.emit_jump(chunk, OP_JMP_IF_FALSE);
                self.emit(chunk, OP_POP);
                self.strict(a, chunk)?;
                let to_end = self.emit_jump(chunk, OP_JMP);
                self.patch_jump(chunk, to_else)?;
                self.emit(chunk, OP_POP);
                self.strict(b, chunk)?;
                self.patch_jump(chunk, to_end)?;
            }
            Expr::Let(binds, body) => {
                let n = Self::count(binds.len(), "bindings in one let")?;
                self.scopes.push(Scope { names: binds.iter().map(|(n, _, _)| n.clone()).collect(), with: false });
                self.emit(chunk, OP_ENV_NEW);
                self.emit_u16(chunk, n);
                for (i, (_, _, init)) in binds.iter().enumerate() {
                    self.binding(init, i, chunk)?;
                    self.emit(chunk, OP_ENV_SET);
                    self.emit_u16(chunk, (i + 1) as u16);
                }
                self.strict(body, chunk)?;
                self.emit(chunk, OP_ENV_POP);
                self.scopes.pop();
            }
            Expr::Lambda(Param::Ident(param), _, body) => {
                // The body runs in a fresh environment whose element 1 is the
                // argument and whose parent is the closure's environment -- the
                // same shape as a one-binding let, so the scope chain is the same.
                self.chunks.push(Chunk::default());
                let body_chunk = self.chunks.len() - 1;
                self.scopes.push(Scope { names: vec![param.clone()], with: false });
                self.strict(body, body_chunk)?;
                self.emit_return(body_chunk);
                self.scopes.pop();
                self.emit(chunk, OP_LAMBDA);
                self.emit_u16(chunk, body_chunk as u16);
            }
            Expr::Lambda(Param::Formals { entries, bind, .. }, pos, body) => {
                // { a, b ? d } @ s: body  ==  s: let a = s.a; b = s.b or d; in body
                // Lazy, defaults may refer to other formals. The closed-formals
                // check ("unexpected argument") is not performed.
                let arg = bind.clone().unwrap_or_else(|| FORMALS_ARG.to_string());
                let binds = entries
                    .iter()
                    .map(|(f, fpos, default)| {
                        let sel = Expr::Select(
                            Box::new(Expr::Var(arg.clone(), *fpos)),
                            vec![f.clone()],
                            default.clone().map(Box::new),
                        );
                        (f.clone(), *fpos, sel)
                    })
                    .collect();
                // Nix forces the argument set before anything else, so a
                // throwing argument fails the call even if no formal is used.
                let force_arg = Expr::Var(arg.clone(), *pos);
                let desugared = Expr::Lambda(
                    Param::Ident(arg),
                    *pos,
                    Box::new(Expr::Seq(Box::new(force_arg), Box::new(Expr::Let(binds, body.clone())))),
                );
                self.strict(&desugared, chunk)?;
            }
            Expr::Seq(a, b) => {
                self.strict(a, chunk)?;
                self.emit(chunk, OP_POP);
                self.strict(b, chunk)?;
            }
            // `import ./f.nix` in a strict position: build the closed thunk and
            // force it, which evaluates the file once and memoises it in place.
            Expr::Import(path, e) => {
                self.closed_thunk(path, e, chunk)?;
                self.emit(chunk, OP_FORCE);
            }
            Expr::Apply(f, arg) => {
                self.strict(f, chunk)?;
                // Arguments are lazy: a literal or a variable is passed by pointer,
                // anything else becomes a thunk (tvix's rule).
                self.lazy(arg, chunk)?;
                self.emit(chunk, OP_CALL);
                self.last_call.insert(chunk, self.chunks[chunk].code.len() - 1);
            }
            Expr::Assert(cond, body) => {
                self.strict(cond, chunk)?;
                self.emit(chunk, OP_ASSERT);
                self.strict(body, chunk)?;
            }
            Expr::Str(parts) => match parts.as_slice() {
                [] => self.emit_const(chunk, Const::Str(String::new()))?,
                [StrPart::Lit(s)] => self.emit_const(chunk, Const::Str(s.clone()))?,
                _ => {
                    for p in parts {
                        match p {
                            StrPart::Lit(s) => self.emit_const(chunk, Const::Str(s.clone()))?,
                            StrPart::Interp(e) => self.strict(e, chunk)?,
                        }
                    }
                    let n = Self::count(parts.len(), "string parts")?;
                    self.emit(chunk, OP_INTERPOLATE);
                    self.emit_u16(chunk, n);
                }
            },
            Expr::List(items) => {
                for it in items {
                    self.lazy(it, chunk)?;
                }
                let n = Self::count(items.len(), "list elements")?;
                self.emit(chunk, OP_LIST);
                self.emit_u16(chunk, n);
            }
            Expr::AttrSet(binds) => {
                // Nix keeps attributes sorted by name bytes; the VM relies on it.
                let mut sorted: Vec<&(String, Pos, Expr)> = binds.iter().collect();
                sorted.sort_by(|a, b| a.0.as_bytes().cmp(b.0.as_bytes()));
                for (name, _, value) in sorted {
                    self.emit_const(chunk, Const::Str(name.clone()))?;
                    self.lazy(value, chunk)?;
                }
                let n = Self::count(binds.len(), "attributes")?;
                self.emit(chunk, OP_ATTRS);
                self.emit_u16(chunk, n);
            }
            Expr::Select(base, path, default) => {
                // builtins.NAME is a primop reference unless `builtins` is shadowed.
                if let (Expr::Var(b, pos), [name], None) = (&**base, path.as_slice(), default) {
                    if b == "builtins" && !self.is_shadowed("builtins") {
                        return self.emit_builtin(name, *pos, chunk);
                    }
                    // Only a `with` stands in the way: it might define
                    // `builtins`, which is not knowable here, so ask at run
                    // time and fall back to the primop. Selecting from the
                    // whole synthesised set would be correct too, but it would
                    // build 47 attributes and 73 chunks at every site.
                    if b == "builtins" && !self.is_lexically_bound("builtins") {
                        self.emit_const(chunk, Const::Str("builtins".to_string()))?;
                        self.emit(chunk, OP_TRY_WITH_GET);
                        let miss = self.emit_jump(chunk, OP_JMP_IF_NOTFOUND);
                        // Found a `builtins` in the with-set: select from it.
                        self.emit_const(chunk, Const::Str(name.clone()))?;
                        self.emit(chunk, OP_SELECT);
                        let end = self.emit_jump(chunk, OP_JMP);
                        self.patch_jump(chunk, miss)?;
                        self.emit(chunk, OP_POP);
                        self.emit_builtin(name, *pos, chunk)?;
                        self.patch_jump(chunk, end)?;
                        return Ok(());
                    }
                }
                self.strict(base, chunk)?;
                match default {
                    None => {
                        for name in path {
                            self.emit_const(chunk, Const::Str(name.clone()))?;
                            self.emit(chunk, OP_SELECT);
                        }
                    }
                    Some(d) => {
                        // Any missing step (or a non-set) takes the default.
                        let mut misses = Vec::new();
                        for name in path {
                            self.emit_const(chunk, Const::Str(name.clone()))?;
                            self.emit(chunk, OP_TRY_SELECT);
                            misses.push(self.emit_jump(chunk, OP_JMP_IF_NOTFOUND));
                        }
                        let to_end = self.emit_jump(chunk, OP_JMP);
                        for m in misses {
                            self.patch_jump(chunk, m)?;
                        }
                        self.emit(chunk, OP_POP);
                        self.strict(d, chunk)?;
                        self.patch_jump(chunk, to_end)?;
                    }
                }
            }
            Expr::HasAttr(base, path) => {
                self.strict(base, chunk)?;
                let (last, init) = path.split_last().expect("non-empty attrpath");
                let mut misses = Vec::new();
                for name in init {
                    self.emit_const(chunk, Const::Str(name.clone()))?;
                    self.emit(chunk, OP_TRY_SELECT);
                    misses.push(self.emit_jump(chunk, OP_JMP_IF_NOTFOUND));
                }
                self.emit_const(chunk, Const::Str(last.clone()))?;
                self.emit(chunk, OP_HAS_ATTR);
                if !misses.is_empty() {
                    let to_end = self.emit_jump(chunk, OP_JMP);
                    for m in misses {
                        self.patch_jump(chunk, m)?;
                    }
                    self.emit(chunk, OP_POP);
                    self.emit(chunk, OP_FALSE);
                    self.patch_jump(chunk, to_end)?;
                }
            }
            Expr::Update(a, b) => {
                self.strict(a, chunk)?;
                self.strict(b, chunk)?;
                self.emit(chunk, OP_UPDATE);
            }
            Expr::Concat(a, b) => {
                self.strict(a, chunk)?;
                self.strict(b, chunk)?;
                self.emit(chunk, OP_CONCAT);
            }
        }
        Ok(())
    }

    /// Compiles `e` in a lazy position: a literal directly, a variable as a
    /// pointer copy (its slot is set by the time this runs), a lambda or primop
    /// as the value it already is, anything else as a thunk in its own chunk.
    fn lazy(&mut self, e: &Expr, chunk: usize) -> Result<(), Unsupported> {
        match e {
            Expr::Int(_) | Expr::Path(_) | Expr::Lambda(..) => return self.strict(e, chunk),
            Expr::Str(parts) if matches!(parts.as_slice(), [] | [StrPart::Lit(_)]) => {
                return self.strict(e, chunk)
            }
            Expr::Var(name, pos) => match self.resolve(name, *pos)? {
                Binding::Literal(op) => {
                    self.emit(chunk, op);
                    return Ok(());
                }
                Binding::Global(g) => return self.emit_builtin(g, *pos, chunk),
                Binding::BuiltinsSet => return self.emit_builtins_set(*pos, chunk),
                Binding::Env(d, i) => {
                    self.emit(chunk, OP_ENV_GET);
                    self.emit_u16(chunk, d);
                    self.emit_u16(chunk, i);
                    return Ok(());
                }
                // A with-lookup forces the with set, so it stays deferred.
                Binding::With(_) => return self.lazy_thunk(e, chunk),
            },
            Expr::Select(base, path, None) => {
                if let (Expr::Var(b, pos), [name]) = (&**base, path.as_slice()) {
                    if b == "builtins" && !self.is_shadowed("builtins") {
                        return self.emit_builtin(name, *pos, chunk);
                    }
                }
            }
            _ => {}
        }
        self.lazy_thunk(e, chunk)
    }

    /// Compiles a `let` binding's initialiser: like `lazy`, except that a bare
    /// reference to a LATER binding in the same let (or to itself) still goes
    /// through a thunk -- its slot is null at this point -- and `inherit x` reads
    /// the scope outside the let.
    fn binding(&mut self, init: &Expr, idx: usize, chunk: usize) -> Result<(), Unsupported> {
        match init {
            Expr::Var(name, pos) => match self.resolve(name, *pos)? {
                Binding::Env(d, i) if d == 0 && (i as usize) > idx => {
                    return self.lazy_thunk(init, chunk);
                }
                _ => return self.lazy(init, chunk),
            },
            Expr::Inherit(name, pos) => match self.resolve_from(name, 1, *pos)? {
                Binding::Literal(op) => {
                    self.emit(chunk, op);
                    return Ok(());
                }
                Binding::Global(g) => return self.emit_builtin(g, *pos, chunk),
                Binding::BuiltinsSet => return self.emit_builtins_set(*pos, chunk),
                Binding::Env(d, i) => {
                    self.emit(chunk, OP_ENV_GET);
                    self.emit_u16(chunk, d);
                    self.emit_u16(chunk, i);
                    return Ok(());
                }
                Binding::With(_) => return self.lazy_thunk(init, chunk),
            },
            _ => self.lazy(init, chunk),
        }
    }

    fn lazy_thunk(&mut self, e: &Expr, chunk: usize) -> Result<(), Unsupported> {
        self.thunk_over(e, chunk, OP_THUNK)
    }

    /// An imported file: its own chunk, reached through a CLOSED_THUNK so it is
    /// evaluated in the EMPTY environment. This IR resolves names against
    /// whatever encloses them, so an ordinary THUNK would let the imported file
    /// see the importer's bindings (docs/BYTECODE.md "Imports").
    fn closed_thunk(&mut self, path: &str, e: &Expr, chunk: usize) -> Result<(), Unsupported> {
        // One chunk per FILE, not per import site. The VM memoises the thunk by
        // chunk index, so this is what makes `import ./f == import ./f` true --
        // both sites name the same chunk and get the same object.
        if let Some(&body) = self.imported.get(path) {
            self.emit(chunk, OP_CLOSED_THUNK);
            self.emit_u16(chunk, body as u16);
            return Ok(());
        }
        // Claim the chunk and record it BEFORE compiling into it: an imported
        // file makes nested thunks of its own, so asking afterwards which chunk
        // was "just" created answers with the innermost one.
        self.chunks.push(Chunk::default());
        let body = self.chunks.len() - 1;
        self.imported.insert(path.to_string(), body);
        // Compiled with NO enclosing scopes, which is the whole point: a free
        // variable in an imported file is then refused here as unbound, the way
        // Nix refuses it, instead of resolving to a (depth, index) that means
        // nothing inside a chunk whose environment is null. Without this the
        // mistake is still caught -- CLOSED_THUNK leaves ENV_GET nothing to
        // walk and the VM stops with E_BADNBC -- but at run time, and a fixture
        // that compiles ought to be a valid program.
        let outer = std::mem::take(&mut self.scopes);
        let r = self.strict(e, body);
        self.scopes = outer;
        r?;
        self.emit_return(body);
        self.emit(chunk, OP_CLOSED_THUNK);
        self.emit_u16(chunk, body as u16);
        Ok(())
    }

    fn thunk_over(&mut self, e: &Expr, chunk: usize, op: u8) -> Result<(), Unsupported> {
        self.chunks.push(Chunk::default());
        let body = self.chunks.len() - 1;
        self.strict(e, body)?;
        self.emit_return(body);
        self.emit(chunk, op);
        self.emit_u16(chunk, body as u16);
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::front::parse;

    fn c(s: &str) -> Vec<Chunk> {
        compile(&parse(s).unwrap()).unwrap()
    }

    #[test]
    fn arithmetic_is_strict_and_deduplicates_constants() {
        let ch = c("1 + 2 * 1");
        assert_eq!(ch.len(), 1);
        assert_eq!(ch[0].consts, vec![Const::Int(1), Const::Int(2)]);
        assert_eq!(
            ch[0].code,
            vec![OP_CONST, 0, 0, OP_CONST, 1, 0, OP_CONST, 0, 0, OP_MUL, OP_ADD, OP_RETURN]
        );
    }

    #[test]
    fn let_places_thunks_only_where_needed() {
        // x: computed -> thunk. y: bare reference to an EARLIER binding -> copy.
        // z: bare reference to a LATER binding -> thunk. w: literal -> literal.
        let ch = c("let x = 1 + 2; y = x; z = w; w = 4; in x");
        assert_eq!(ch.len(), 3, "one chunk each for x's and z's thunks");
        assert_eq!(
            ch[0].code,
            vec![
                OP_ENV_NEW, 4, 0,
                OP_THUNK, 1, 0, OP_ENV_SET, 1, 0,
                OP_ENV_GET, 0, 0, 1, 0, OP_ENV_SET, 2, 0,
                OP_THUNK, 2, 0, OP_ENV_SET, 3, 0,
                OP_CONST, 0, 0, OP_ENV_SET, 4, 0,
                OP_ENV_GET, 0, 0, 1, 0, OP_FORCE,
                OP_ENV_POP, OP_RETURN,
            ]
        );
        assert_eq!(ch[1].code, vec![OP_CONST, 0, 0, OP_CONST, 1, 0, OP_ADD, OP_RETURN]);
        assert_eq!(ch[2].code, vec![OP_ENV_GET, 0, 0, 4, 0, OP_FORCE, OP_RETURN]);
    }

    #[test]
    fn nested_lets_resolve_by_depth() {
        let ch = c("let a = 1; in let b = 2; in a");
        // inner body: ENV_GET depth 1, index 1
        let code = &ch[0].code;
        let at = code.windows(5).position(|w| w == [OP_ENV_GET, 1, 0, 1, 0]).unwrap();
        assert_eq!(code[at + 5], OP_FORCE);
    }

    #[test]
    fn if_and_short_circuit_shapes() {
        let ch = c("if true then 1 else 2");
        assert_eq!(
            ch[0].code,
            vec![
                OP_TRUE,
                OP_JMP_IF_FALSE, 7, 0,   // -> the else POP
                OP_POP, OP_CONST, 0, 0,
                OP_JMP, 4, 0,            // -> end
                OP_POP, OP_CONST, 1, 0,
                OP_RETURN,
            ]
        );
        let ch = c("false && true");
        assert_eq!(ch[0].code, vec![OP_FALSE, OP_JMP_IF_FALSE, 2, 0, OP_POP, OP_TRUE, OP_RETURN]);
        let ch = c("false -> true");
        assert_eq!(
            ch[0].code,
            vec![OP_FALSE, OP_NOT, OP_JMP_IF_TRUE, 2, 0, OP_POP, OP_TRUE, OP_RETURN]
        );
    }

    #[test]
    fn shadowing_the_globals_works() {
        let ch = c("let true = 1; in true");
        assert!(ch[0].code.windows(5).any(|w| w == [OP_ENV_GET, 0, 0, 1, 0]));
        assert!(!ch[0].code.contains(&OP_TRUE));
    }

    #[test]
    fn unbound_names_are_refused() {
        assert!(compile(&parse("x + 1").unwrap()).is_err());
    }

    #[test]
    fn lambdas_and_lazy_arguments() {
        // (x: x + a) 41 inside a let: the body chunk reads x at depth 0 and a at
        // depth 1; the literal argument is passed directly, no thunk.
        let ch = c("let a = 1; in (x: x + a) 41");
        assert_eq!(ch.len(), 2);
        assert_eq!(
            ch[1].code,
            vec![OP_ENV_GET, 0, 0, 1, 0, OP_FORCE, OP_ENV_GET, 1, 0, 1, 0, OP_FORCE, OP_ADD, OP_RETURN]
        );
        assert!(ch[0].code.windows(4).any(|w| w == [OP_CONST, 1, 0, OP_CALL])); // k0 = 1, k1 = 41
        // a computed argument becomes a thunk; a variable argument is a pointer copy
        let ch = c("let f = x: x; y = 2; in f (y + 1)");
        assert!(ch[0].code.windows(4).any(|w| w[0] == OP_THUNK && w[3] == OP_CALL));
        let ch = c("let f = x: x; y = 2; in f y");
        assert!(ch[0].code.windows(6).any(|w| w == [OP_ENV_GET, 0, 0, 2, 0, OP_CALL]));
        // assert
        let ch = c("assert true; 1");
        assert_eq!(ch[0].code, vec![OP_TRUE, OP_ASSERT, OP_CONST, 0, 0, OP_RETURN]);
    }

    #[test]
    fn strings_lists_and_sets() {
        let ch = c("\"a${\"b\"}c\"");
        assert_eq!(
            ch[0].code,
            vec![OP_CONST, 0, 0, OP_CONST, 1, 0, OP_CONST, 2, 0, OP_INTERPOLATE, 3, 0, OP_RETURN]
        );
        // list elements are lazy: the literal stays inline, the sum is a thunk
        let ch = c("[ 1 (1 + 1) ]");
        assert_eq!(ch[0].code, vec![OP_CONST, 0, 0, OP_THUNK, 1, 0, OP_LIST, 2, 0, OP_RETURN]);
        // attributes are emitted sorted by name bytes, names as string constants
        let ch = c("{ b = 1; a = 2; \"B\" = 3; }");
        assert_eq!(ch[0].consts, vec![Const::Str("B".into()), Const::Int(3), Const::Str("a".into()), Const::Int(2), Const::Str("b".into()), Const::Int(1)]);
        assert_eq!(ch[0].code[ch[0].code.len() - 4..], [OP_ATTRS, 3, 0, OP_RETURN]);
        // a.b.c or d: two try-selects, both misses jump to the default
        let ch = c("let a = { }; in a.b.c or 7");
        let code = &ch[0].code;
        assert_eq!(code.iter().filter(|&&b| b == OP_TRY_SELECT).count(), 2);
        assert_eq!(code.iter().filter(|&&b| b == OP_JMP_IF_NOTFOUND).count(), 2);
        assert!(code.windows(3).any(|w| w[0] == OP_POP && w[1] == OP_CONST));
        // a ? b is a single HAS_ATTR
        let ch = c("let a = { }; in a ? b");
        assert!(ch[0].code.windows(4).any(|w| w[0] == OP_CONST && w[3] == OP_HAS_ATTR));
        assert!(!ch[0].code.contains(&OP_TRY_SELECT));
        // inherit in a let reads the OUTER scope: depth 1, not itself
        let ch = c("let x = 1; in let inherit x; in x");
        assert!(ch[0].code.windows(8).any(|w| w == [OP_ENV_GET, 1, 0, 1, 0, OP_ENV_SET, 1, 0]));
    }

    #[test]
    fn derivation_is_the_embedded_file_once() {
        // Every spelling of `derivation` is the CLOSED_THUNK of ONE chunk,
        // forced: derivation.nix compiled like an import, so every site gets
        // the same function (docs/DERIVATION.md).
        let ch = c("[ derivation builtins.derivation (with { }; derivation) ]");
        let sites: Vec<u16> = ch
            .iter()
            .flat_map(|k| k.code.windows(4).filter(|w| w[0] == OP_CLOSED_THUNK && w[3] == OP_FORCE)
                .map(|w| u16::from_le_bytes([w[1], w[2]])).collect::<Vec<_>>())
            .collect();
        assert_eq!(sites.len(), 3, "three sites");
        assert!(sites.iter().all(|&s| s == sites[0]), "one chunk: {sites:?}");
        // ...whose body calls the primop, which is a global as in Nix 2.34.
        assert!(ch.iter().any(|k| k.code.windows(2).any(|w| w == [OP_PRIM, 0x31])));
        // A shadowed `derivation` is an ordinary variable.
        assert!(c("let derivation = 1; in derivation").iter().all(|k| !k.code.contains(&OP_CLOSED_THUNK)));
    }

    #[test]
    fn builtins_are_synthesised_lambdas() {
        // builtins.length xs: one lambda whose body is ENV_GET 0,1; PRIM length; RETURN
        let ch = c("builtins.length [ ]");
        let (id, _) = primop("length").unwrap();
        assert_eq!(ch[1].code, vec![OP_ENV_GET, 0, 0, 1, 0, OP_PRIM, id, OP_RETURN]);
        assert!(ch[0].code.windows(3).any(|w| w == [OP_LAMBDA, 1, 0]));
        // a two-argument primop is two nested lambdas; the inner one pushes
        // outermost-first. Unqualified `map` resolves to the same chunk.
        let ch = c("[ (builtins.map (x: x) [ ]) (map (x: x) [ ]) ]");
        let (id, _) = primop("map").unwrap();
        let inner = ch.iter().position(|k| k.code == [OP_ENV_GET, 1, 0, 1, 0, OP_ENV_GET, 0, 0, 1, 0, OP_PRIM, id, OP_RETURN]).unwrap();
        let outer = ch.iter().position(|k| k.code == [OP_LAMBDA, inner as u8, 0, OP_RETURN]).unwrap();
        assert_eq!(ch.iter().filter(|k| k.code == [OP_LAMBDA, inner as u8, 0, OP_RETURN]).count(), 1, "synthesised once");
        let _ = outer;
        // shadowed builtins is an ordinary variable; unknown primops are refused by name
        assert!(c("let builtins = { length = 1; }; in builtins.length").iter().all(|k| !k.code.contains(&OP_PRIM)));
        assert_eq!(compile(&parse("builtins.nope").unwrap()).unwrap_err().what, "builtins.nope");
        // A value builtin is a constant, not a primop lambda (D23).
        assert!(c("builtins.currentSystem").iter().all(|k| !k.code.contains(&OP_LAMBDA)));
    }

    #[test]
    fn with_formals_and_tail_calls() {
        // an identifier no let binds, under a with, is a runtime lookup; a
        // lexical binding wins over it; a global is the fallback
        let ch = c("let x = 1; in with { y = 2; }; x + y");
        assert_eq!(
            ch[0].code,
            vec![
                OP_ENV_NEW, 1, 0, OP_CONST, 0, 0, OP_ENV_SET, 1, 0,
                OP_THUNK, 1, 0, OP_WITH_PUSH,              // the set stays lazy
                OP_ENV_GET, 1, 0, 1, 0, OP_FORCE,          // x: depth 1, past the with env
                OP_CONST, 1, 0, OP_WITH_GET, OP_FORCE,     // y
                OP_ADD, OP_ENV_POP, OP_ENV_POP, OP_RETURN,
            ]
        );
        assert_eq!(ch[1].code, vec![OP_CONST, 0, 0, OP_CONST, 1, 0, OP_ATTRS, 1, 0, OP_RETURN]);
        let ch = c("with { }; map");
        assert!(ch[0].code.contains(&OP_TRY_WITH_GET) && ch[0].code.contains(&OP_JMP_IF_NOTFOUND));
        // formals desugar to a let over selections; the default uses `or`
        let ch = c("({ a, b ? a + 1 } @ s: b) { a = 1; }");
        assert!(ch.iter().any(|k| k.code.contains(&OP_TRY_SELECT)));
        assert!(ch.iter().any(|k| k.code.contains(&OP_SELECT)));
        // a call right before RETURN is a tail call; one inside `then` is not
        let ch = c("let f = n: if n == 0 then 0 else f (n - 1); in f 3");
        let body = ch.iter().find(|k| k.code.contains(&OP_TAIL_CALL)).expect("tail call emitted");
        assert_eq!(body.code[body.code.len() - 2..], [OP_TAIL_CALL, OP_RETURN]);
        assert_eq!(ch[0].code[ch[0].code.len() - 3..], [OP_CALL, OP_ENV_POP, OP_RETURN]);
    }
}
