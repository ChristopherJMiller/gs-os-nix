//! A recursive-descent parser for the slice-1 subset, following Nix's own
//! precedence table (src/libexpr/parser.y):
//!
//!   ->  (1, right)   ||  (2, left)   &&  (3, left)   == !=  (4, none)
//!   < > <= >=  (5, none)   [// 6, right]   !  (7, prefix)   [++ 8, right]
//!   + -  (9, left)   * /  (10, left)   [? 11]   unary -  (12)   [application, select]
//!
//! `let` and `if` are "function-level" forms: they extend as far right as they can
//! and appear as operands only inside parentheses -- exactly as in Nix, where
//! `1 + if c then 1 else 2` is a syntax error.

use crate::lexer::{unsupported, Pos, Tok, Unsupported};

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
}

pub fn parse(tokens: &[(Tok, Pos)]) -> Result<Expr, Unsupported> {
    let mut p = Parser { toks: tokens, i: 0 };
    let e = p.expr()?;
    if p.peek() != &Tok::Eof {
        return unsupported(p.pos(), format!("trailing {:?}", p.peek()));
    }
    Ok(e)
}

struct Parser<'a> {
    toks: &'a [(Tok, Pos)],
    i: usize,
}

#[derive(Clone, Copy, PartialEq)]
enum Assoc {
    Left,
    Right,
    None,
}

fn binary(tok: &Tok) -> Option<(u8, Assoc)> {
    Some(match tok {
        Tok::Implies => (1, Assoc::Right),
        Tok::OrOr => (2, Assoc::Left),
        Tok::AndAnd => (3, Assoc::Left),
        Tok::EqEq | Tok::NotEq => (4, Assoc::None),
        Tok::Lt | Tok::Gt | Tok::Le | Tok::Ge => (5, Assoc::None),
        Tok::Plus | Tok::Minus => (9, Assoc::Left),
        Tok::Star | Tok::Slash => (10, Assoc::Left),
        _ => return None,
    })
}

const PREC_NOT: u8 = 7;
const PREC_NEG: u8 = 12;

impl<'a> Parser<'a> {
    fn peek(&self) -> &Tok {
        &self.toks[self.i].0
    }
    fn pos(&self) -> Pos {
        self.toks[self.i].1
    }
    fn next(&mut self) -> (Tok, Pos) {
        let t = self.toks[self.i].clone();
        if self.i + 1 < self.toks.len() {
            self.i += 1;
        }
        t
    }
    fn expect(&mut self, want: Tok, ctx: &str) -> Result<(), Unsupported> {
        if self.peek() == &want {
            self.next();
            Ok(())
        } else {
            unsupported(self.pos(), format!("expected {want:?} {ctx}, found {:?}", self.peek()))
        }
    }

    /// Function-level expression: let, if, or an operator expression.
    fn expr(&mut self) -> Result<Expr, Unsupported> {
        match self.peek() {
            Tok::Let => {
                self.next();
                let mut binds: Vec<(String, Pos, Expr)> = Vec::new();
                loop {
                    match self.next() {
                        (Tok::In, _) => break,
                        (Tok::Ident(name), pos) => {
                            if self.peek() != &Tok::Assign {
                                return unsupported(pos, "attribute path or inherit in let");
                            }
                            self.next();
                            let init = self.expr()?;
                            self.expect(Tok::Semi, "after let binding")?;
                            if binds.iter().any(|(n, _, _)| n == &name) {
                                return unsupported(pos, format!("`{name}` bound twice in one let"));
                            }
                            binds.push((name, pos, init));
                        }
                        (t, pos) => return unsupported(pos, format!("{t:?} in let bindings")),
                    }
                }
                let body = self.expr()?;
                Ok(Expr::Let(binds, Box::new(body)))
            }
            Tok::If => {
                self.next();
                let c = self.expr()?;
                self.expect(Tok::Then, "in if")?;
                let a = self.expr()?;
                self.expect(Tok::Else, "in if")?;
                let b = self.expr()?;
                Ok(Expr::If(Box::new(c), Box::new(a), Box::new(b)))
            }
            _ => self.op(0),
        }
    }

    /// Precedence climbing over the binary operators.
    fn op(&mut self, min_prec: u8) -> Result<Expr, Unsupported> {
        let mut lhs = self.prefix()?;
        loop {
            let tok = self.peek().clone();
            let Some((prec, assoc)) = binary(&tok) else { break };
            if prec < min_prec {
                break;
            }
            let pos = self.pos();
            self.next();
            let rhs_min = match assoc {
                Assoc::Left | Assoc::None => prec + 1,
                Assoc::Right => prec,
            };
            let rhs = self.op(rhs_min)?;
            lhs = match tok {
                Tok::Implies => Expr::Implies(Box::new(lhs), Box::new(rhs)),
                Tok::OrOr => Expr::Or(Box::new(lhs), Box::new(rhs)),
                Tok::AndAnd => Expr::And(Box::new(lhs), Box::new(rhs)),
                Tok::EqEq => Expr::Bin(BinOp::Eq, Box::new(lhs), Box::new(rhs)),
                Tok::NotEq => Expr::Bin(BinOp::Ne, Box::new(lhs), Box::new(rhs)),
                Tok::Lt => Expr::Bin(BinOp::Lt, Box::new(lhs), Box::new(rhs)),
                Tok::Le => Expr::Bin(BinOp::Le, Box::new(lhs), Box::new(rhs)),
                Tok::Gt => Expr::Bin(BinOp::Gt, Box::new(lhs), Box::new(rhs)),
                Tok::Ge => Expr::Bin(BinOp::Ge, Box::new(lhs), Box::new(rhs)),
                Tok::Plus => Expr::Bin(BinOp::Add, Box::new(lhs), Box::new(rhs)),
                Tok::Minus => Expr::Bin(BinOp::Sub, Box::new(lhs), Box::new(rhs)),
                Tok::Star => Expr::Bin(BinOp::Mul, Box::new(lhs), Box::new(rhs)),
                Tok::Slash => Expr::Bin(BinOp::Div, Box::new(lhs), Box::new(rhs)),
                _ => unreachable!(),
            };
            // Non-associative operators may not chain: `a == b == c` is a Nix
            // syntax error, and so is `a < b < c`.
            if assoc == Assoc::None {
                if let Some((p2, _)) = binary(self.peek()) {
                    if p2 == prec {
                        return unsupported(pos, "chained non-associative operator");
                    }
                }
            }
        }
        Ok(lhs)
    }

    /// Prefix operators. `!` binds looser than `+` (so `!a + b` is `!(a + b)`);
    /// unary `-` binds tighter than everything but application.
    fn prefix(&mut self) -> Result<Expr, Unsupported> {
        match self.peek() {
            Tok::Bang => {
                self.next();
                let e = self.op(PREC_NOT + 1)?;
                Ok(Expr::Not(Box::new(e)))
            }
            Tok::Minus => {
                self.next();
                // Fold `-<int>` so `let x = -1;` is a literal binding, not a thunk.
                if let Tok::Int(v) = self.peek().clone() {
                    if let Some(neg) = v.checked_neg() {
                        self.next();
                        return Ok(Expr::Int(neg));
                    }
                }
                let e = self.op(PREC_NEG + 1)?;
                Ok(Expr::Neg(Box::new(e)))
            }
            _ => self.atom(),
        }
    }

    fn atom(&mut self) -> Result<Expr, Unsupported> {
        let (tok, pos) = self.next();
        let e = match tok {
            Tok::Int(v) => Expr::Int(v),
            Tok::Ident(name) => Expr::Var(name, pos),
            Tok::LParen => {
                let e = self.expr()?;
                self.expect(Tok::RParen, "to close parenthesis")?;
                e
            }
            t => return unsupported(pos, format!("{t:?} where an expression was expected")),
        };
        // Juxtaposition is function application, which is not in this slice.
        match self.peek() {
            Tok::Int(_) | Tok::Ident(_) | Tok::LParen => {
                unsupported(self.pos(), "function application")
            }
            _ => Ok(e),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::lexer::lex;

    fn p(s: &str) -> Expr {
        parse(&lex(s).unwrap()).unwrap()
    }
    fn var(s: &str) -> Box<Expr> {
        Box::new(Expr::Var(s.into(), Pos { line: 1, col: 0 }))
    }
    fn int(v: i64) -> Box<Expr> {
        Box::new(Expr::Int(v))
    }
    // Positions are irrelevant to structure; strip them for comparison.
    fn strip(e: Expr) -> Expr {
        match e {
            Expr::Var(n, _) => Expr::Var(n, Pos { line: 1, col: 0 }),
            Expr::Neg(a) => Expr::Neg(Box::new(strip(*a))),
            Expr::Not(a) => Expr::Not(Box::new(strip(*a))),
            Expr::Bin(o, a, b) => Expr::Bin(o, Box::new(strip(*a)), Box::new(strip(*b))),
            Expr::And(a, b) => Expr::And(Box::new(strip(*a)), Box::new(strip(*b))),
            Expr::Or(a, b) => Expr::Or(Box::new(strip(*a)), Box::new(strip(*b))),
            Expr::Implies(a, b) => Expr::Implies(Box::new(strip(*a)), Box::new(strip(*b))),
            Expr::If(c, a, b) => {
                Expr::If(Box::new(strip(*c)), Box::new(strip(*a)), Box::new(strip(*b)))
            }
            Expr::Let(bs, body) => Expr::Let(
                bs.into_iter().map(|(n, _, e)| (n, Pos { line: 1, col: 0 }, strip(e))).collect(),
                Box::new(strip(*body)),
            ),
            e => e,
        }
    }

    #[test]
    fn precedence_matches_nix() {
        // * over +, left-assoc
        assert_eq!(
            strip(p("1 + 2 * 3 - 4")),
            Expr::Bin(
                BinOp::Sub,
                Box::new(Expr::Bin(BinOp::Add, int(1), Box::new(Expr::Bin(BinOp::Mul, int(2), int(3))))),
                int(4)
            )
        );
        // ! is looser than +: !a + b == !(a + b)
        assert_eq!(
            strip(p("!a + b")),
            Expr::Not(Box::new(Expr::Bin(BinOp::Add, var("a"), var("b"))))
        );
        // but tighter than ==: !a == b == (!a) == b
        assert_eq!(
            strip(p("!a == b")),
            Expr::Bin(BinOp::Eq, Box::new(Expr::Not(var("a"))), var("b"))
        );
        // unary minus is tight: -a * b == (-a) * b
        assert_eq!(
            strip(p("-a * b")),
            Expr::Bin(BinOp::Mul, Box::new(Expr::Neg(var("a"))), var("b"))
        );
        // -> is right-assoc and lowest
        assert_eq!(
            strip(p("a -> b -> c || d")),
            Expr::Implies(var("a"), Box::new(Expr::Implies(var("b"), Box::new(Expr::Or(var("c"), var("d"))))))
        );
        // literal folding
        assert_eq!(strip(p("-5")), Expr::Int(-5));
    }

    #[test]
    fn let_and_if() {
        assert_eq!(
            strip(p("let x = 1; y = x; in if y then 2 else 3")),
            Expr::Let(
                vec![
                    ("x".into(), Pos { line: 1, col: 0 }, Expr::Int(1)),
                    ("y".into(), Pos { line: 1, col: 0 }, *var("x")),
                ],
                Box::new(Expr::If(var("y"), int(2), int(3)))
            )
        );
    }

    #[test]
    fn refuses_outside_the_subset() {
        for s in ["a == b == c", "f x", "1 + if true then 1 else 2", "let x = 1; x = 2; in x", "let inherit x; in x"] {
            assert!(lex(s).and_then(|t| parse(&t)).is_err(), "{s}");
        }
    }
}
