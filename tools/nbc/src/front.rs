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

use crate::ast::{unsupported, BinOp, Expr, Param, Pos, StrPart, Unsupported};
use rnix::ast::{self, AstNode, BinOpKind, HasEntry, LiteralKind, UnaryOpKind};
use std::path::{Path, PathBuf};

/// Where relative and home path VALUES resolve: the convention Nix's own
/// language tests (and tvix's corpus) are written against, so the expected
/// outputs match and no fixture carries this machine's directories (D7).
/// Imports are different: they read a real file, relative to the importer.
pub const FIXTURE_PWD: &str = "/pwd/lang";
pub const FIXTURE_HOME: &str = "/fake-home";

/// Nix's canonPath: absolute, no `.` or empty components, `..` resolved (and
/// clamped at the root), no trailing slash except for `/` itself. The VM's
/// path `+` does the same at run time (target/vm/c/data.c path_join).
pub fn canon_path(p: &str) -> String {
    let mut parts: Vec<&str> = Vec::new();
    for c in p.split('/') {
        match c {
            "" | "." => {}
            ".." => {
                parts.pop();
            }
            c => parts.push(c),
        }
    }
    if parts.is_empty() {
        "/".to_string()
    } else {
        format!("/{}", parts.join("/"))
    }
}

/// Parse a file, so `import ./other.nix` can be resolved relative to it. The
/// generator resolves imports itself; the VM has no parser and no filesystem
/// (docs/BYTECODE.md "Imports").
pub fn parse_file(path: &Path) -> Result<Expr, Unsupported> {
    let src = match std::fs::read_to_string(path) {
        Ok(s) => s,
        Err(e) => return unsupported(Pos { line: 0, col: 0 }, format!("cannot read {}: {e}", path.display())),
    };
    let here = path.canonicalize().unwrap_or_else(|_| path.to_path_buf());
    parse_in(&src, here.parent().map(|d| d.to_path_buf()), vec![here])
}

/// Parse a bare string, with no file to resolve imports against: the unit
/// tests, and the embedded derivation.nix. An import in a string is refused
/// rather than guessed.
pub fn parse(src: &str) -> Result<Expr, Unsupported> {
    parse_in(src, None, Vec::new())
}

fn parse_in(src: &str, dir: Option<PathBuf>, chain: Vec<PathBuf>) -> Result<Expr, Unsupported> {
    let parsed = rnix::Root::parse(src);
    if let Some(err) = parsed.errors().first() {
        return unsupported(Pos { line: 0, col: 0 }, format!("parse error: {err}"));
    }
    let root = parsed.tree();
    let Some(expr) = root.expr() else {
        return unsupported(Pos { line: 0, col: 0 }, "empty file");
    };
    Lower { src, dir, chain }.expr(&expr)
}

struct Lower<'a> {
    src: &'a str,
    /// The directory of the file being lowered, for resolving a relative
    /// import. `None` when parsing a bare string (the unit tests), which makes
    /// every import a refusal rather than a guess.
    dir: Option<PathBuf>,
    /// Files currently being imported, outermost first, so a cycle is refused
    /// instead of recursing forever.
    chain: Vec<PathBuf>,
}

impl<'a> Lower<'a> {
    /// Is this the bare identifier `import`?
    fn is_import(&self, f: &ast::Expr) -> bool {
        matches!(f, ast::Expr::Ident(id)
            if id.ident_token().map(|t| t.text() == "import").unwrap_or(false))
    }

    /// `import <path literal>`: read that file, lower it, and wrap it so the
    /// compiler gives it its own chunk and a CLOSED_THUNK. Only a literal
    /// relative or absolute path can be resolved here -- anything computed is
    /// refused (docs/BYTECODE.md "Imports").
    fn import(&self, app: &ast::Apply, arg: &ast::Expr) -> Result<Expr, Unsupported> {
        let text = match arg {
            ast::Expr::PathRel(p) => p.to_string(),
            ast::Expr::PathAbs(p) => p.to_string(),
            _ => return self.refuse(app, "computed import"),
        };
        if text.contains("${") {
            return self.refuse(app, "interpolated import path");
        }
        let Some(dir) = self.dir.as_ref() else {
            return self.refuse(app, "import with no file to resolve against");
        };
        let joined = if text.starts_with('/') {
            PathBuf::from(&text)
        } else {
            dir.join(&text)
        };
        // A directory import means `<dir>/default.nix`, as Nix does.
        let target = if joined.is_dir() { joined.join("default.nix") } else { joined };
        let target = target.canonicalize().unwrap_or(target);
        if self.chain.contains(&target) {
            return self.refuse(app, "import cycle");
        }
        let src = match std::fs::read_to_string(&target) {
            Ok(s) => s,
            Err(e) => return self.refuse(app, &format!("import {}: {e}", target.display())),
        };
        let key = target.to_string_lossy().into_owned();
        let mut chain = self.chain.clone();
        let dir = target.parent().map(|d| d.to_path_buf());
        chain.push(target);
        // A refusal from the imported file carries ITS position, which would
        // point into the wrong file, so re-anchor it here and keep the name.
        match parse_in(&src, dir, chain) {
            Ok(e) => Ok(Expr::Import(key, Box::new(e))),
            Err(u) => self.refuse(app, &format!("in imported file: {}", u.what)),
        }
    }

    fn pos(&self, node: &impl AstNode) -> Pos {
        let off: usize = node.syntax().text_range().start().into();
        let before = &self.src[..off.min(self.src.len())];
        let line = before.matches('\n').count() + 1;
        let col = before.rsplit('\n').next().map(|l| l.chars().count()).unwrap_or(0) + 1;
        Pos { line, col }
    }

    /// A path literal as a value: joined to `base` if relative, then made
    /// canonical, as Nix does when it parses one. Interpolated paths
    /// (`./a/${b}`) are not yet supported and are refused by name.
    fn path_value(&self, node: &impl AstNode, text: &str, base: Option<&str>) -> Result<Expr, Unsupported> {
        if text.contains("${") {
            return self.refuse(node, "path antiquotation");
        }
        let joined = match base {
            Some(b) => format!("{b}/{text}"),
            None => text.to_string(),
        };
        Ok(Expr::Path(canon_path(&joined)))
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

    /// `rec { a = 1; b = a; }`  ==  `let a = 1; b = a; in { inherit a b; }`.
    /// `inherit x;` inside it reads the scope outside, as in a let.
    fn rec_set(&self, node: &impl HasEntry, what: &str) -> Result<Expr, Unsupported> {
        let binds = self.bindings(node, what, true)?;
        let set = binds
            .iter()
            .map(|(n, pos, _)| (n.clone(), *pos, Expr::Var(n.clone(), *pos)))
            .collect();
        Ok(Expr::Let(binds, Box::new(Expr::AttrSet(set))))
    }

    /// Adds `name = value` to a binding list, merging attribute-set literals so
    /// that `a.b = 1; a.c = 2;` and `a = { b = 1; }; a.c = 2;` both become
    /// `a = { b = 1; c = 2; }`. Anything else landing twice is refused.
    fn insert_binding(
        binds: &mut Vec<(String, Pos, Expr)>,
        name: String,
        pos: Pos,
        value: Expr,
        what: &str,
    ) -> Result<(), Unsupported> {
        if let Some(existing) = binds.iter_mut().find(|(n, _, _)| *n == name) {
            match (&mut existing.2, value) {
                (Expr::AttrSet(old), Expr::AttrSet(new)) => {
                    for (n, p, v) in new {
                        Self::insert_binding(old, n, p, v, what)?;
                    }
                    Ok(())
                }
                _ => unsupported(pos, format!("`{name}` bound twice in one {what}")),
            }
        } else {
            binds.push((name, pos, value));
            Ok(())
        }
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
            Self::insert_binding(&mut binds, name, pos, e, what)
        };
        for entry in node.attrpath_values() {
            let Some(path) = entry.attrpath() else { return self.refuse(&entry, "binding without a name") };
            let names = self.static_path(&path)?;
            let Some(value) = entry.value() else { return self.refuse(&entry, "binding without a value") };
            let pos = self.pos(&path);
            // a.b.c = v  ==  a = { b = { c = v; }; }, merged with its siblings
            let mut value = self.expr(&value)?;
            let (first, rest) = names.split_first().expect("non-empty attrpath");
            for n in rest.iter().rev() {
                value = Expr::AttrSet(vec![(n.clone(), pos, value)]);
            }
            push(first.clone(), pos, value)?;
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
                // `import` reaching here is `import` used as anything other
                // than the head of `import <path literal>` -- passed around,
                // or applied to something the generator cannot resolve. Either
                // way it cannot be resolved at compile time, and guessing would
                // produce a fixture that runs and answers wrongly.
                Some(tok) if tok.text() == "import" => self.refuse(id, "import as a value"),
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
                // The lowering has no scope tracking, so it cannot tell a
                // shadowed `import` from the builtin. Refuse rather than pick.
                if binds.iter().any(|(n, _, _)| n == "import") {
                    return self.refuse(l, "import is shadowed");
                }
                let Some(body) = l.body() else { return self.refuse(l, "let without a body") };
                Ok(Expr::Let(binds, Box::new(self.expr(&body)?)))
            }
            ast::Expr::AttrSet(s) => {
                if s.rec_token().is_some() {
                    return self.rec_set(s, "attribute set");
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
                        Ok(Expr::Lambda(Param::Ident(name), self.pos(&p), Box::new(self.expr(&body)?)))
                    }
                    Some(ast::Param::Pattern(p)) => {
                        let mut entries = Vec::new();
                        for e in p.pat_entries() {
                            let name = match e.ident().and_then(|i| i.ident_token()) {
                                Some(tok) => tok.text().to_string(),
                                None => return self.refuse(&e, "identifier `or` as formal"),
                            };
                            let default = match e.default() {
                                Some(d) => Some(self.expr(&d)?),
                                None => None,
                            };
                            entries.push((name, self.pos(&e), default));
                        }
                        let bind = match p.pat_bind().and_then(|b| b.ident()).and_then(|i| i.ident_token()) {
                            Some(tok) => Some(tok.text().to_string()),
                            None => None,
                        };
                        let formals = Param::Formals { entries, ellipsis: p.ellipsis_token().is_some(), bind };
                        Ok(Expr::Lambda(formals, self.pos(&p), Box::new(self.expr(&body)?)))
                    }
                    None => self.refuse(lam, "lambda without a parameter"),
                }
            }
            ast::Expr::With(w) => {
                let (Some(ns), Some(body)) = (w.namespace(), w.body()) else {
                    return self.refuse(w, "incomplete with");
                };
                Ok(Expr::With(Box::new(self.expr(&ns)?), Box::new(self.expr(&body)?)))
            }
            ast::Expr::LegacyLet(l) => {
                // let { a = 1; body = a; }  ==  (rec { a = 1; body = a; }).body
                let set = self.rec_set(l, "let")?;
                Ok(Expr::Select(Box::new(set), vec!["body".into()], None))
            }
            ast::Expr::Apply(app) => {
                let (Some(f), Some(a)) = (app.lambda(), app.argument()) else {
                    return self.refuse(app, "incomplete application");
                };
                if self.is_import(&f) {
                    return self.import(app, &a);
                }
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
            // A path VALUE resolves against the fixture convention (FIXTURE_PWD,
            // FIXTURE_HOME), never the disk: the expected outputs are written
            // that way, and a fixture must not carry this machine's paths (D7).
            ast::Expr::PathAbs(n) => self.path_value(n, &n.to_string(), None),
            ast::Expr::PathRel(n) => self.path_value(n, &n.to_string(), Some(FIXTURE_PWD)),
            ast::Expr::PathHome(n) => {
                let text = n.to_string();
                self.path_value(n, text.trim_start_matches('~'), Some(FIXTURE_HOME))
            }
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
            Expr::Lambda(n, _, b) => Expr::Lambda(strip_param(n), Pos { line: 1, col: 0 }, Box::new(strip(*b))),
            Expr::With(e, b) => Expr::With(Box::new(strip(*e)), Box::new(strip(*b))),
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
            Expr::Seq(a, b) => Expr::Seq(Box::new(strip(*a)), Box::new(strip(*b))),
            e => e,
        }
    }
    fn strip_param(p: Param) -> Param {
        match p {
            Param::Formals { entries, ellipsis, bind } => Param::Formals {
                entries: entries.into_iter().map(|(n, _, d)| (n, Pos { line: 1, col: 0 }, d.map(strip))).collect(),
                ellipsis,
                bind,
            },
            p => p,
        }
    }
    fn ident(s: &str) -> Param {
        Param::Ident(s.into())
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

    /// Nix's canonPath, and the fixture convention for relative and home paths.
    /// The expected values are Nix's own (tvix corpus eval-okay-dirof and
    /// eval-okay-path-antiquotation), not derived from this code.
    #[test]
    fn path_literals_are_absolute_and_canonical() {
        assert_eq!(canon_path("/"), "/");
        assert_eq!(canon_path("/."), "/");
        assert_eq!(canon_path("/foo/bar/"), "/foo/bar");
        assert_eq!(canon_path("//a//b/./c/.."), "/a/b");
        assert_eq!(canon_path("/../.."), "/");
        assert_eq!(canon_path("/foo/bar/../xyzzy/./foo.txt"), "/foo/xyzzy/foo.txt");
        assert_eq!(parse("/foo/bar").unwrap(), Expr::Path("/foo/bar".into()));
        assert_eq!(parse("./foo").unwrap(), Expr::Path("/pwd/lang/foo".into()));
        assert_eq!(parse("../x").unwrap(), Expr::Path("/pwd/x".into()));
        assert_eq!(parse("~/foo").unwrap(), Expr::Path("/fake-home/foo".into()));
    }

    #[test]
    fn refuses_by_feature_name() {
        let cases = [
            ("{ ${a} = 1; }", "dynamic attribute name"),
            ("{ \"${a}\" = 1; }", "dynamic attribute name"),
            ("./p/${a}", "path antiquotation"),
            ("<nixpkgs>", "search path literal"),
            ("1.5", "float literal"),
            ("let x = 1; x = 2; in x", "`x` bound twice in one let"),
            ("{ a = 1; inherit a; }", "`a` bound twice in one attribute set"),
            // Imports the generator will not guess at (docs/BYTECODE.md).
            ("import ./lib.nix", "import with no file to resolve against"),
            ("import (f 1)", "computed import"),
            ("let import = 1; in 2", "import is shadowed"),
            ("[ import ]", "import as a value"),
            ("map import [ 1 ]", "import as a value"),
        ];
        for (src, want) in cases {
            let err = parse(src).unwrap_err();
            assert_eq!(err.what, want, "{src}");
        }
        assert!(parse("a == b == c").unwrap_err().what.starts_with("parse error"));
        assert!(parse("1 +").unwrap_err().what.starts_with("parse error"));
    }

    /// An import is resolved against the importing FILE, and the imported file is
    /// lowered in its own right -- so this needs real files, not a string.
    #[test]
    fn resolves_an_import_relative_to_the_file() {
        let dir = std::env::temp_dir().join(format!("nbc-import-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("lib.nix"), "{ a = 1; }\n").unwrap();
        std::fs::write(dir.join("main.nix"), "(import ./lib.nix).a\n").unwrap();
        // A file that reads a name the IMPORTER binds. Nix refuses it, and so
        // must this: the imported file is compiled with no enclosing scope, so
        // the name is unbound rather than captured.
        std::fs::write(dir.join("peek.nix"), "secret\n").unwrap();
        std::fs::write(dir.join("capture.nix"), "let secret = 9; in import ./peek.nix\n").unwrap();
        // And one that imports itself.
        std::fs::write(dir.join("loop.nix"), "import ./loop.nix\n").unwrap();

        // Two sites importing the same file, one of them through a symlink.
        std::fs::write(dir.join("twice.nix"),
                       "[ (import ./lib.nix) (import ./link.nix) ]\n").unwrap();
        let _ = std::os::unix::fs::symlink("lib.nix", dir.join("link.nix"));

        let ast = parse_file(&dir.join("main.nix")).unwrap();
        // The import became its own subtree, not an Apply of a variable.
        assert!(matches!(&ast, Expr::Select(inner, _, _) if matches!(**inner, Expr::Import(..))),
                "expected a Select over an Import, got {ast:?}");

        // One file is one chunk however many sites import it, and a symlink is
        // the same file -- which is what makes `import ./f == import ./f` true
        // (docs/BYTECODE.md "Imports").
        let twice = parse_file(&dir.join("twice.nix")).unwrap();
        let chunks = crate::compiler::compile(&twice).unwrap();
        // List elements are lazy, so each site sits in its own element chunk.
        let closed: Vec<u16> = chunks
            .iter()
            .flat_map(|k| {
                k.code
                    .windows(3)
                    .filter(|w| w[0] == crate::opcodes::OP_CLOSED_THUNK)
                    .map(|w| u16::from_le_bytes([w[1], w[2]]))
                    .collect::<Vec<_>>()
            })
            .collect();
        assert_eq!(closed.len(), 2, "expected two import sites");
        assert_eq!(closed[0], closed[1], "both sites must name the same chunk");

        let captured = parse_file(&dir.join("capture.nix")).unwrap();
        assert_eq!(crate::compiler::compile(&captured).unwrap_err().what,
                   "unbound variable `secret`");

        assert_eq!(parse_file(&dir.join("loop.nix")).unwrap_err().what, "import cycle");
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn positions_are_line_and_column() {
        let err = parse("let\n  x = 1;\n  in <x>").unwrap_err();   // any refused construct
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
                ident("x"),
                Pos { line: 1, col: 0 },
                Box::new(Expr::Lambda(
                    ident("y"),
                    Pos { line: 1, col: 0 },
                    Box::new(Expr::Bin(BinOp::Add, var("x"), var("y")))
                ))
            )
        );
        assert_eq!(p("assert true; 1"), Expr::Assert(var("true"), int(1)));
        // formals
        assert_eq!(
            p("{ a, b ? 1, ... } @ s: a"),
            Expr::Lambda(
                Param::Formals {
                    entries: vec![("a".into(), Pos { line: 1, col: 0 }, None), ("b".into(), Pos { line: 1, col: 0 }, Some(Expr::Int(1)))],
                    ellipsis: true,
                    bind: Some("s".into()),
                },
                Pos { line: 1, col: 0 },
                var("a")
            )
        );
    }

    #[test]
    fn desugars_rec_legacy_let_and_attr_paths() {
        assert_eq!(
            p("rec { a = 1; b = a; }"),
            Expr::Let(
                vec![("a".into(), Pos { line: 1, col: 0 }, Expr::Int(1)), ("b".into(), Pos { line: 1, col: 0 }, *var("a"))],
                Box::new(Expr::AttrSet(vec![
                    ("a".into(), Pos { line: 1, col: 0 }, *var("a")),
                    ("b".into(), Pos { line: 1, col: 0 }, *var("b")),
                ]))
            )
        );
        assert_eq!(
            p("let { body = 1; }"),
            Expr::Select(
                Box::new(Expr::Let(
                    vec![("body".into(), Pos { line: 1, col: 0 }, Expr::Int(1))],
                    Box::new(Expr::AttrSet(vec![("body".into(), Pos { line: 1, col: 0 }, *var("body"))]))
                )),
                vec!["body".into()],
                None
            )
        );
        assert_eq!(
            p("{ a.b = 1; a.c = 2; a = { d = 3; }; }"),
            Expr::AttrSet(vec![(
                "a".into(),
                Pos { line: 1, col: 0 },
                Expr::AttrSet(vec![
                    ("b".into(), Pos { line: 1, col: 0 }, Expr::Int(1)),
                    ("c".into(), Pos { line: 1, col: 0 }, Expr::Int(2)),
                    ("d".into(), Pos { line: 1, col: 0 }, Expr::Int(3)),
                ])
            )])
        );
        assert_eq!(p("with a; b"), Expr::With(var("a"), var("b")));
        assert!(parse("{ a.b = 1; a = 2; }").is_err());
        assert!(parse("{ a.b = 1; a.b = 2; }").is_err());
    }
}
