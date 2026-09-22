//! rnix -> the generator's AST.
//!
//! rnix parses all of Nix (it is the parser tvix uses), so precedence, comments,
//! strings, paths and every other lexical question are settled upstream. This
//! module walks its tree and produces `ast::Expr` for the slice the VM can run,
//! refusing everything else with the NAME of the construct -- the scoreboard
//! histograms those names, and that is how the next slice is chosen.
//!
//! The refusals are deliberate and complete: an unsupported node is never
//! silently approximated, because a fixture that compiles wrongly produces a
//! wrong-but-plausible answer, which is the failure mode this project guards
//! against hardest.

use crate::ast::{unsupported, BinOp, Expr, Pos, StrPart, Unsupported};
use rnix::ast::{self, AstNode, BinOpKind, HasEntry, LiteralKind, UnaryOpKind};

pub fn parse(src: &str) -> Result<Expr, Unsupported> {
    let parsed = rnix::Root::parse(src);
    if let Some(err) = parsed.errors().first() {
        return unsupported(Pos { line: 0, col: 0 }, format!("parse error: {err}"));
    }
    let root = parsed.tree();
    let Some(expr) = root.expr() else {
        return unsupported(Pos { line: 0, col: 0 }, "empty file");
    };
    Lower { src }.expr(&expr)
}

struct Lower<'a> {
    src: &'a str,
}

impl<'a> Lower<'a> {
    fn pos(&self, node: &impl AstNode) -> Pos {
        let off: usize = node.syntax().text_range().start().into();
        let before = &self.src[..off.min(self.src.len())];
        let line = before.matches('\n').count() + 1;
        let col = before.rsplit('\n').next().map(|l| l.chars().count()).unwrap_or(0) + 1;
        Pos { line, col }
    }

    fn refuse<T>(&self, node: &impl AstNode, what: &str) -> Result<T, Unsupported> {
        unsupported(self.pos(node), what)
    }

    /// A static attribute name: an identifier, or a quoted string without
    /// interpolation. `${…}` keys are dynamic and not in this slice.
    fn static_attr(&self, attr: &ast::Attr) -> Result<String, Unsupported> {
        match attr {
            ast::Attr::Ident(id) => match id.ident_token() {
                Some(tok) => Ok(tok.text().to_string()),
                None => self.refuse(id, "identifier `or` as attribute name"),
            },
            ast::Attr::Str(s) => {
                let mut name = String::new();
                for part in s.normalized_parts() {
                    match part {
                        ast::InterpolPart::Literal(t) => name.push_str(&t),
                        ast::InterpolPart::Interpolation(_) => {
                            return self.refuse(s, "dynamic attribute name")
                        }
                    }
                }
                Ok(name)
            }
            ast::Attr::Dynamic(d) => self.refuse(d, "dynamic attribute name"),
        }
    }

    fn static_path(&self, path: &ast::Attrpath) -> Result<Vec<String>, Unsupported> {
        path.attrs().map(|a| self.static_attr(&a)).collect()
    }

    /// The bindings of a `let` or an attribute set: `name = value;`, `inherit
    /// a b;` and `inherit (e) a b;`. Nested paths (`a.b = 1;`) are not in this
    /// slice. In a `let`, `inherit a;` refers to the scope OUTSIDE the let.
    fn bindings(
        &self,
        node: &impl HasEntry,
        what: &str,
        is_let: bool,
    ) -> Result<Vec<(String, Pos, Expr)>, Unsupported> {
        let mut binds: Vec<(String, Pos, Expr)> = Vec::new();
        let mut push = |name: String, pos: Pos, e: Expr| -> Result<(), Unsupported> {
            if binds.iter().any(|(n, _, _)| *n == name) {
                return unsupported(pos, format!("`{name}` bound twice in one {what}"));
            }
            binds.push((name, pos, e));
            Ok(())
        };
        for entry in node.attrpath_values() {
            let Some(path) = entry.attrpath() else { return self.refuse(&entry, "binding without a name") };
            let attrs: Vec<ast::Attr> = path.attrs().collect();
            let name = match attrs.as_slice() {
                [one] => self.static_attr(one)?,
                _ => return unsupported(self.pos(&path), format!("attribute path in {what}")),
            };
            let Some(value) = entry.value() else { return self.refuse(&entry, "binding without a value") };
            push(name, self.pos(&path), self.expr(&value)?)?;
        }
        for inh in node.inherits() {
            let from = match inh.from() {
                Some(f) => match f.expr() {
                    Some(e) => Some(self.expr(&e)?),
                    None => return self.refuse(&f, "empty inherit source"),
                },
                None => None,
            };
            for attr in inh.attrs() {
                let name = self.static_attr(&attr)?;
                let pos = self.pos(&attr);
                let e = match &from {
                    Some(src) => Expr::Select(Box::new(src.clone()), vec![name.clone()], None),
                    None if is_let => Expr::Inherit(name.clone(), pos),
                    None => Expr::Var(name.clone(), pos),
                };
                push(name, pos, e)?;
            }
        }
        Ok(binds)
    }

    fn expr(&self, e: &ast::Expr) -> Result<Expr, Unsupported> {
        match e {
            ast::Expr::Literal(lit) => match lit.kind() {
                LiteralKind::Integer(i) => match i.value() {
                    Ok(v) => Ok(Expr::Int(v)),
                    Err(_) => self.refuse(lit, "integer literal out of range"),
                },
                LiteralKind::Float(_) => self.refuse(lit, "float literal"),
                LiteralKind::Uri(_) => self.refuse(lit, "URI literal"),
            },
            ast::Expr::Ident(id) => match id.ident_token() {
                Some(tok) => Ok(Expr::Var(tok.text().to_string(), self.pos(id))),
                None => self.refuse(id, "identifier `or`"),
            },
            ast::Expr::Paren(p) => match p.expr() {
                Some(inner) => self.expr(&inner),
                None => self.refuse(p, "empty parentheses"),
            },
            ast::Expr::UnaryOp(u) => {
                let Some(operand) = u.expr() else { return self.refuse(u, "unary operator without operand") };
                match u.operator() {
                    Some(UnaryOpKind::Negate) => {
                        // Fold `-<int>` so `let x = -1;` is a literal binding, not a thunk.
                        if let ast::Expr::Literal(lit) = &operand {
                            if let LiteralKind::Integer(i) = lit.kind() {
                                if let Some(v) = i.value().ok().and_then(i64::checked_neg) {
                                    return Ok(Expr::Int(v));
                                }
                            }
                        }
                        Ok(Expr::Neg(Box::new(self.expr(&operand)?)))
                    }
                    Some(UnaryOpKind::Invert) => Ok(Expr::Not(Box::new(self.expr(&operand)?))),
                    None => self.refuse(u, "unknown unary operator"),
                }
            }
            ast::Expr::BinOp(b) => {
                let (Some(l), Some(r)) = (b.lhs(), b.rhs()) else {
                    return self.refuse(b, "binary operator missing an operand");
                };
                let op = match b.operator() {
                    Some(BinOpKind::Add) => BinOp::Add,
                    Some(BinOpKind::Sub) => BinOp::Sub,
                    Some(BinOpKind::Mul) => BinOp::Mul,
                    Some(BinOpKind::Div) => BinOp::Div,
                    Some(BinOpKind::Equal) => BinOp::Eq,
                    Some(BinOpKind::NotEqual) => BinOp::Ne,
                    Some(BinOpKind::Less) => BinOp::Lt,
                    Some(BinOpKind::LessOrEq) => BinOp::Le,
                    Some(BinOpKind::More) => BinOp::Gt,
                    Some(BinOpKind::MoreOrEq) => BinOp::Ge,
                    Some(BinOpKind::And) => {
                        return Ok(Expr::And(Box::new(self.expr(&l)?), Box::new(self.expr(&r)?)))
                    }
                    Some(BinOpKind::Or) => {
                        return Ok(Expr::Or(Box::new(self.expr(&l)?), Box::new(self.expr(&r)?)))
                    }
                    Some(BinOpKind::Implication) => {
                        return Ok(Expr::Implies(Box::new(self.expr(&l)?), Box::new(self.expr(&r)?)))
                    }
                    Some(BinOpKind::Concat) => {
                        return Ok(Expr::Concat(Box::new(self.expr(&l)?), Box::new(self.expr(&r)?)))
                    }
                    Some(BinOpKind::Update) => {
                        return Ok(Expr::Update(Box::new(self.expr(&l)?), Box::new(self.expr(&r)?)))
                    }
                    Some(BinOpKind::PipeRight) | Some(BinOpKind::PipeLeft) => {
                        return self.refuse(b, "pipe operator")
                    }
                    None => return self.refuse(b, "unknown binary operator"),
                };
                Ok(Expr::Bin(op, Box::new(self.expr(&l)?), Box::new(self.expr(&r)?)))
            }
            ast::Expr::IfElse(i) => {
                let (Some(c), Some(t), Some(f)) = (i.condition(), i.body(), i.else_body()) else {
                    return self.refuse(i, "incomplete if");
                };
                Ok(Expr::If(
                    Box::new(self.expr(&c)?),
                    Box::new(self.expr(&t)?),
                    Box::new(self.expr(&f)?),
                ))
            }
            ast::Expr::LetIn(l) => {
                let binds = self.bindings(l, "let", true)?;
                let Some(body) = l.body() else { return self.refuse(l, "let without a body") };
                Ok(Expr::Let(binds, Box::new(self.expr(&body)?)))
            }
            ast::Expr::AttrSet(s) => {
                if s.rec_token().is_some() {
                    return self.refuse(s, "rec attribute set");
                }
                Ok(Expr::AttrSet(self.bindings(s, "attribute set", false)?))
            }
            ast::Expr::List(l) => {
                let mut items = Vec::new();
                for it in l.items() {
                    items.push(self.expr(&it)?);
                }
                Ok(Expr::List(items))
            }
            ast::Expr::Str(s) => {
                let mut parts = Vec::new();
                for part in s.normalized_parts() {
                    match part {
                        ast::InterpolPart::Literal(text) => parts.push(StrPart::Lit(text)),
                        ast::InterpolPart::Interpolation(i) => match i.expr() {
                            Some(e) => parts.push(StrPart::Interp(self.expr(&e)?)),
                            None => return self.refuse(&i, "empty interpolation"),
                        },
                    }
                }
                Ok(Expr::Str(parts))
            }
            ast::Expr::Select(sel) => {
                let (Some(e), Some(path)) = (sel.expr(), sel.attrpath()) else {
                    return self.refuse(sel, "incomplete selection");
                };
                let names = self.static_path(&path)?;
                let default = match sel.default_expr() {
                    Some(d) => Some(Box::new(self.expr(&d)?)),
                    None => None,
                };
                Ok(Expr::Select(Box::new(self.expr(&e)?), names, default))
            }
            ast::Expr::HasAttr(h) => {
                let (Some(e), Some(path)) = (h.expr(), h.attrpath()) else {
                    return self.refuse(h, "incomplete `?`");
                };
                let names = self.static_path(&path)?;
                Ok(Expr::HasAttr(Box::new(self.expr(&e)?), names))
            }
            ast::Expr::Lambda(lam) => {
                let Some(body) = lam.body() else { return self.refuse(lam, "lambda without a body") };
                match lam.param() {
                    Some(ast::Param::IdentParam(p)) => {
                        let name = match p.ident().and_then(|i| i.ident_token()) {
                            Some(tok) => tok.text().to_string(),
                            None => return self.refuse(&p, "identifier `or` as parameter"),
                        };
                        Ok(Expr::Lambda(name, self.pos(&p), Box::new(self.expr(&body)?)))
                    }
                    Some(ast::Param::Pattern(p)) => self.refuse(&p, "formals pattern"),
                    None => self.refuse(lam, "lambda without a parameter"),
                }
            }
            ast::Expr::Apply(app) => {
                let (Some(f), Some(a)) = (app.lambda(), app.argument()) else {
                    return self.refuse(app, "incomplete application");
                };
                Ok(Expr::Apply(Box::new(self.expr(&f)?), Box::new(self.expr(&a)?)))
            }
            ast::Expr::Assert(a) => {
                let (Some(c), Some(b)) = (a.condition(), a.body()) else {
                    return self.refuse(a, "incomplete assert");
                };
                Ok(Expr::Assert(Box::new(self.expr(&c)?), Box::new(self.expr(&b)?)))
            }
            // Everything below is real Nix the VM cannot run yet. Named, so the
            // scoreboard can say which feature would move the most tests.
            ast::Expr::With(n) => self.refuse(n, "with"),
            ast::Expr::LegacyLet(n) => self.refuse(n, "legacy let"),
            ast::Expr::PathAbs(n) => self.refuse(n, "path literal"),
            ast::Expr::PathRel(n) => self.refuse(n, "path literal"),
            ast::Expr::PathHome(n) => self.refuse(n, "path literal"),
            ast::Expr::PathSearch(n) => self.refuse(n, "search path literal"),
            ast::Expr::CurPos(n) => self.refuse(n, "__curPos"),
            ast::Expr::Root(n) => self.refuse(n, "nested root"),
            ast::Expr::Error(n) => self.refuse(n, "syntax error"),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn p(s: &str) -> Expr {
        strip(parse(s).unwrap())
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
            Expr::Lambda(n, _, b) => Expr::Lambda(n, Pos { line: 1, col: 0 }, Box::new(strip(*b))),
            Expr::Apply(f, a) => Expr::Apply(Box::new(strip(*f)), Box::new(strip(*a))),
            Expr::Assert(c, b) => Expr::Assert(Box::new(strip(*c)), Box::new(strip(*b))),
            Expr::Str(parts) => Expr::Str(
                parts
                    .into_iter()
                    .map(|p| match p {
                        StrPart::Interp(e) => StrPart::Interp(strip(e)),
                        lit => lit,
                    })
                    .collect(),
            ),
            Expr::List(items) => Expr::List(items.into_iter().map(strip).collect()),
            Expr::AttrSet(bs) => Expr::AttrSet(
                bs.into_iter().map(|(n, _, e)| (n, Pos { line: 1, col: 0 }, strip(e))).collect(),
            ),
            Expr::Select(e, path, d) => {
                Expr::Select(Box::new(strip(*e)), path, d.map(|d| Box::new(strip(*d))))
            }
            Expr::HasAttr(e, path) => Expr::HasAttr(Box::new(strip(*e)), path),
            Expr::Update(a, b) => Expr::Update(Box::new(strip(*a)), Box::new(strip(*b))),
            Expr::Concat(a, b) => Expr::Concat(Box::new(strip(*a)), Box::new(strip(*b))),
            Expr::Inherit(n, _) => Expr::Inherit(n, Pos { line: 1, col: 0 }),
            e => e,
        }
    }

    // These pin down that the lowering preserves Nix's precedence as rnix parses
    // it -- the same cases the hand-written parser was tested against.
    #[test]
    fn precedence_is_nix_precedence() {
        assert_eq!(
            p("1 + 2 * 3 - 4"),
            Expr::Bin(
                BinOp::Sub,
                Box::new(Expr::Bin(BinOp::Add, int(1), Box::new(Expr::Bin(BinOp::Mul, int(2), int(3))))),
                int(4)
            )
        );
        assert_eq!(p("!a + b"), Expr::Not(Box::new(Expr::Bin(BinOp::Add, var("a"), var("b")))));
        assert_eq!(p("!a == b"), Expr::Bin(BinOp::Eq, Box::new(Expr::Not(var("a"))), var("b")));
        assert_eq!(p("-a * b"), Expr::Bin(BinOp::Mul, Box::new(Expr::Neg(var("a"))), var("b")));
        assert_eq!(
            p("a -> b -> c || d"),
            Expr::Implies(var("a"), Box::new(Expr::Implies(var("b"), Box::new(Expr::Or(var("c"), var("d"))))))
        );
        assert_eq!(p("-5"), Expr::Int(-5));
        assert_eq!(p("- 5 # comment\n/* block */"), Expr::Int(-5));
    }

    #[test]
    fn let_and_if() {
        assert_eq!(
            p("let x = 1; y = x; in if y then 2 else 3"),
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
    fn refuses_by_feature_name() {
        let cases = [
            ("{ a, b }: a", "formals pattern"),
            ("rec { a = 1; }", "rec attribute set"),
            ("{ ${a} = 1; }", "dynamic attribute name"),
            ("{ \"${a}\" = 1; }", "dynamic attribute name"),
            ("with a; b", "with"),
            ("./p", "path literal"),
            ("1.5", "float literal"),
            ("let a.b = 1; in a", "attribute path in let"),
            ("{ a.b = 1; }", "attribute path in attribute set"),
            ("let x = 1; x = 2; in x", "`x` bound twice in one let"),
            ("{ a = 1; inherit a; }", "`a` bound twice in one attribute set"),
        ];
        for (src, want) in cases {
            let err = parse(src).unwrap_err();
            assert_eq!(err.what, want, "{src}");
        }
        assert!(parse("a == b == c").unwrap_err().what.starts_with("parse error"));
        assert!(parse("1 +").unwrap_err().what.starts_with("parse error"));
    }

    #[test]
    fn positions_are_line_and_column() {
        let err = parse("let\n  x = 1;\n  in with x; x").unwrap_err();
        assert_eq!(err.pos, Pos { line: 3, col: 6 });
    }

    #[test]
    fn strings_lists_sets_and_paths() {
        assert_eq!(
            p("\"a${b}c\\n\""),
            Expr::Str(vec![StrPart::Lit("a".into()), StrPart::Interp(*var("b")), StrPart::Lit("c\n".into())])
        );
        // indented strings are dedented by rnix
        assert_eq!(p("''\n  x\n  y\n''"), Expr::Str(vec![StrPart::Lit("x\ny\n".into())]));
        assert_eq!(p("[ 1 x ]"), Expr::List(vec![Expr::Int(1), *var("x")]));
        assert_eq!(
            p("{ b = 1; \"a b\" = 2; inherit c; inherit (d) e; }"),
            Expr::AttrSet(vec![
                ("b".into(), Pos { line: 1, col: 0 }, Expr::Int(1)),
                ("a b".into(), Pos { line: 1, col: 0 }, Expr::Int(2)),
                ("c".into(), Pos { line: 1, col: 0 }, *var("c")),
                ("e".into(), Pos { line: 1, col: 0 }, Expr::Select(var("d"), vec!["e".into()], None)),
            ])
        );
        assert_eq!(
            p("let inherit x; in x"),
            Expr::Let(vec![("x".into(), Pos { line: 1, col: 0 }, Expr::Inherit("x".into(), Pos { line: 1, col: 0 }))], var("x"))
        );
        assert_eq!(p("a.b.c or 1"), Expr::Select(var("a"), vec!["b".into(), "c".into()], Some(int(1))));
        assert_eq!(p("a ? b.\"c\""), Expr::HasAttr(var("a"), vec!["b".into(), "c".into()]));
        assert_eq!(p("a // b ++ c"), Expr::Update(var("a"), Box::new(Expr::Concat(var("b"), var("c")))));
    }

    #[test]
    fn lambdas_application_assert() {
        // application binds tighter than any operator and is left-associative
        assert_eq!(
            p("f a b + 1"),
            Expr::Bin(
                BinOp::Add,
                Box::new(Expr::Apply(Box::new(Expr::Apply(var("f"), var("a"))), var("b"))),
                int(1)
            )
        );
        // a lambda body extends as far right as possible
        assert_eq!(
            p("x: y: x + y"),
            Expr::Lambda(
                "x".into(),
                Pos { line: 1, col: 0 },
                Box::new(Expr::Lambda(
                    "y".into(),
                    Pos { line: 1, col: 0 },
                    Box::new(Expr::Bin(BinOp::Add, var("x"), var("y")))
                ))
            )
        );
        assert_eq!(p("assert true; 1"), Expr::Assert(var("true"), int(1)));
    }
}
