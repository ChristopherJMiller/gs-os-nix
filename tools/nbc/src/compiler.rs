//! AST -> chunks, following the thunk-placement rules in docs/BYTECODE.md.
//!
//! Scopes mirror the VM's environment chain exactly: each `let` pushes one scope,
//! and a variable resolves to (depth, index) where depth counts enclosing lets and
//! index is 1-based within the let (element 0 of an environment is its parent).

use crate::ast::{unsupported, BinOp, Expr, Pos, Unsupported};
use crate::opcodes::*;

#[derive(Debug, Clone, PartialEq)]
pub enum Const {
    Int(i64),
}

#[derive(Debug, Default, Clone, PartialEq)]
pub struct Chunk {
    pub consts: Vec<Const>,
    pub code: Vec<u8>,
}

pub struct Compiler {
    pub chunks: Vec<Chunk>,
    scopes: Vec<Vec<String>>,
}

/// What a name resolved to.
enum Binding {
    /// (depth, index) into the environment chain.
    Env(u16, u16),
    /// The global `true`, `false`, `null`.
    Literal(u8),
}

pub fn compile(program: &Expr) -> Result<Vec<Chunk>, Unsupported> {
    let mut c = Compiler { chunks: vec![Chunk::default()], scopes: Vec::new() };
    c.strict(program, 0)?;
    c.emit(0, OP_RETURN);
    Ok(c.chunks)
}

impl Compiler {
    fn emit(&mut self, chunk: usize, b: u8) {
        self.chunks[chunk].code.push(b);
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
        let rel16 = i16::try_from(rel).map_err(|_| Unsupported {
            pos: Pos { line: 0, col: 0 },
            what: "jump offset out of range".into(),
        })?;
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
        u16::try_from(idx).map_err(|_| Unsupported {
            pos: Pos { line: 0, col: 0 },
            what: "more than 65535 constants in one chunk".into(),
        })
    }

    fn resolve(&self, name: &str, pos: Pos) -> Result<Binding, Unsupported> {
        for (depth, scope) in self.scopes.iter().rev().enumerate() {
            if let Some(i) = scope.iter().position(|n| n == name) {
                return Ok(Binding::Env(depth as u16, (i + 1) as u16));
            }
        }
        match name {
            "true" => Ok(Binding::Literal(OP_TRUE)),
            "false" => Ok(Binding::Literal(OP_FALSE)),
            "null" => Ok(Binding::Literal(OP_NULL)),
            _ => unsupported(pos, format!("unbound variable `{name}`")),
        }
    }

    /// Compiles `e` so that its VALUE is on top of the stack afterwards.
    fn strict(&mut self, e: &Expr, chunk: usize) -> Result<(), Unsupported> {
        match e {
            Expr::Int(v) => {
                let idx = self.constant(chunk, Const::Int(*v))?;
                self.emit(chunk, OP_CONST);
                self.emit_u16(chunk, idx);
            }
            Expr::Var(name, pos) => match self.resolve(name, *pos)? {
                Binding::Literal(op) => self.emit(chunk, op),
                Binding::Env(d, i) => {
                    self.emit(chunk, OP_ENV_GET);
                    self.emit_u16(chunk, d);
                    self.emit_u16(chunk, i);
                    self.emit(chunk, OP_FORCE);
                }
            },
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
                let n = u16::try_from(binds.len()).map_err(|_| Unsupported {
                    pos: Pos { line: 0, col: 0 },
                    what: "more than 65535 bindings in one let".into(),
                })?;
                self.scopes.push(binds.iter().map(|(n, _, _)| n.clone()).collect());
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
            Expr::Lambda(param, _, body) => {
                // The body runs in a fresh environment whose element 1 is the
                // argument and whose parent is the closure's environment -- the
                // same shape as a one-binding let, so the scope chain is the same.
                self.chunks.push(Chunk::default());
                let body_chunk = self.chunks.len() - 1;
                self.scopes.push(vec![param.clone()]);
                self.strict(body, body_chunk)?;
                self.emit(body_chunk, OP_RETURN);
                self.scopes.pop();
                self.emit(chunk, OP_LAMBDA);
                self.emit_u16(chunk, body_chunk as u16);
            }
            Expr::Apply(f, arg) => {
                self.strict(f, chunk)?;
                // Arguments are lazy: a literal or a variable is passed by pointer,
                // anything else becomes a thunk (tvix's rule).
                self.lazy(arg, chunk)?;
                self.emit(chunk, OP_CALL);
            }
            Expr::Assert(cond, body) => {
                self.strict(cond, chunk)?;
                self.emit(chunk, OP_ASSERT);
                self.strict(body, chunk)?;
            }
        }
        Ok(())
    }

    /// Compiles `e` in a lazy position: a literal directly, a variable as a
    /// pointer copy (its slot is set by the time this runs), anything else as a
    /// thunk in its own chunk.
    fn lazy(&mut self, e: &Expr, chunk: usize) -> Result<(), Unsupported> {
        match e {
            Expr::Int(_) => return self.strict(e, chunk),
            Expr::Var(name, pos) => match self.resolve(name, *pos)? {
                Binding::Literal(op) => {
                    self.emit(chunk, op);
                    return Ok(());
                }
                Binding::Env(d, i) => {
                    self.emit(chunk, OP_ENV_GET);
                    self.emit_u16(chunk, d);
                    self.emit_u16(chunk, i);
                    return Ok(());
                }
            },
            _ => {}
        }
        self.chunks.push(Chunk::default());
        let body = self.chunks.len() - 1;
        self.strict(e, body)?;
        self.emit(body, OP_RETURN);
        self.emit(chunk, OP_THUNK);
        self.emit_u16(chunk, body as u16);
        Ok(())
    }

    /// Compiles a `let` binding's initialiser: a literal directly, an already-bound
    /// variable as a pointer copy, anything else as a thunk. `idx` is the binding's
    /// 0-based position, so a bare reference to a LATER binding in the same let (or
    /// to itself) still goes through a thunk -- its slot is null at this point.
    fn binding(&mut self, init: &Expr, idx: usize, chunk: usize) -> Result<(), Unsupported> {
        match init {
            Expr::Int(_) => return self.strict(init, chunk),
            Expr::Var(name, pos) => match self.resolve(name, *pos)? {
                Binding::Literal(op) => {
                    self.emit(chunk, op);
                    return Ok(());
                }
                Binding::Env(d, i) if d > 0 || (i as usize) <= idx => {
                    self.emit(chunk, OP_ENV_GET);
                    self.emit_u16(chunk, d);
                    self.emit_u16(chunk, i);
                    return Ok(());
                }
                Binding::Env(..) => {}
            },
            _ => {}
        }
        // A thunk: its own chunk, compiled in the current scope (it captures the
        // let's environment), ending in RETURN.
        self.chunks.push(Chunk::default());
        let body = self.chunks.len() - 1;
        self.strict(init, body)?;
        self.emit(body, OP_RETURN);
        self.emit(chunk, OP_THUNK);
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
}
