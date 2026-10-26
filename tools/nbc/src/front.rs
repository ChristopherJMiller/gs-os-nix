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

use crate::ast::{unsupported, BinOp, Expr, Key, Param, Pos, StrPart, Unsupported};
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
    /// relative or absolute path can be resolved here; anything computed is
    /// the `import` primop, applied at run time, where reading a file is
    /// GS/OS's (D26; docs/BYTECODE.md "Imports").
    fn import(&self, app: &ast::Apply, arg: &ast::Expr) -> Result<Expr, Unsupported> {
        let text = match arg {
            ast::Expr::PathRel(p) => p.to_string(),
            ast::Expr::PathAbs(p) => p.to_string(),
            _ => {
                let pos = self.pos(app);
                return Ok(Expr::Apply(Box::new(Expr::Var("import".into(), pos)), Box::new(self.expr(arg)?)));
            }
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

    /// An attribute name: an identifier, a quoted string, or `${e}`. Nix's
    /// parser makes `${"a"}` -- a plain string literal, in parentheses or
    /// not, `"…"` or `''…''` -- the static name `a`; `"…${e}…"` and any other
    /// `${e}` are computed at run time (BYTECODE.md "Dynamic attribute names").
    fn attr_key(&self, attr: &ast::Attr) -> Result<Key, Unsupported> {
        match attr {
            ast::Attr::Ident(id) => match id.ident_token() {
                Some(tok) => Ok(Key::Name(tok.text().to_string())),
                None => self.refuse(id, "identifier `or` as attribute name"),
            },
            ast::Attr::Str(s) => match Self::literal_str(s) {
                Some(name) => Ok(Key::Name(name)),
                None => Ok(Key::Expr(self.expr(&ast::Expr::Str(s.clone()))?)),
            },
            ast::Attr::Dynamic(d) => {
                let Some(mut e) = d.expr() else { return self.refuse(d, "empty `${}` attribute name") };
                while let ast::Expr::Paren(p) = &e {
                    match p.expr() {
                        Some(inner) => e = inner,
                        None => return self.refuse(p, "empty parentheses"),
                    }
                }
                if let ast::Expr::Str(s) = &e {
                    if let Some(name) = Self::literal_str(s) {
                        return Ok(Key::Name(name));
                    }
                }
                Ok(Key::Expr(self.expr(&e)?))
            }
        }
    }

    /// A string with no interpolation: its text.
    fn literal_str(s: &ast::Str) -> Option<String> {
        let mut name = String::new();
        for part in s.normalized_parts() {
            match part {
                ast::InterpolPart::Literal(t) => name.push_str(&t),
                ast::InterpolPart::Interpolation(_) => return None,
            }
        }
        Some(name)
    }

    /// A name that must be static: in an `inherit`, Nix's parse error otherwise.
    fn static_attr(&self, attr: &ast::Attr) -> Result<String, Unsupported> {
        match self.attr_key(attr)? {
            Key::Name(n) => Ok(n),
            Key::Expr(_) => self.refuse(attr, "dynamic attributes not allowed in inherit"),
        }
    }

    fn path_keys(&self, path: &ast::Attrpath) -> Result<Vec<Key>, Unsupported> {
        path.attrs().map(|a| self.attr_key(&a)).collect()
    }

    /// `rec { a = 1; b = a; }`  ==  `let a = 1; b = a; in { inherit a b; }`.
    /// `inherit x;` inside it reads the scope outside, as in a let. Computed
    /// names, and their values, are in the let's body: they see the set's
    /// attributes, and are not themselves in scope.
    fn rec_set(&self, node: &impl HasEntry, what: &str) -> Result<Expr, Unsupported> {
        let (binds, dyns) = self.bindings(node, what, true)?;
        Ok(Expr::RecSet(binds, dyns))
    }

    /// A set literal: AttrSet, or DynAttrSet when a name is computed.
    fn set_of(statics: Vec<(String, Pos, Expr)>, dyns: Vec<(Expr, Pos, Expr)>) -> Expr {
        if dyns.is_empty() { Expr::AttrSet(statics) } else { Expr::DynAttrSet(statics, dyns) }
    }

    /// A set literal's static and computed attributes and whether it is rec,
    /// or the expression back.
    #[allow(clippy::type_complexity)]
    fn set_parts(e: Expr) -> Result<(Vec<(String, Pos, Expr)>, Vec<(Expr, Pos, Expr)>, bool), Expr> {
        match e {
            Expr::AttrSet(s) => Ok((s, Vec::new(), false)),
            Expr::DynAttrSet(s, d) => Ok((s, d, false)),
            Expr::RecSet(s, d) => Ok((s, d, true)),
            e => Err(e),
        }
    }

    /// Adds `name = value` to a binding list, merging attribute-set literals so
    /// that `a.b = 1; a.c = 2;` and `a = { b = 1; }; a.c = 2;` both become
    /// `a = { b = 1; c = 2; }`; their computed names are kept, unmerged, as
    /// Nix does. The first one's rec-ness is the merged set's, as in Nix's
    /// parser: `a = rec { x = 1; }; a = { y = x; };` is one rec set, where y
    /// is 1, and `a = { x = 1; }; a = rec { y = x; };` is not, where x is the
    /// one outside. Anything else landing twice is refused.
    fn insert_binding(
        binds: &mut Vec<(String, Pos, Expr)>,
        name: String,
        pos: Pos,
        value: Expr,
        what: &str,
    ) -> Result<(), Unsupported> {
        if let Some(existing) = binds.iter_mut().find(|(n, _, _)| *n == name) {
            let old = std::mem::replace(&mut existing.2, Expr::Int(0));
            match (Self::set_parts(old), Self::set_parts(value)) {
                (Ok((mut s1, mut d1, rec1)), Ok((s2, d2, rec2))) => {
                    for (n, p, v) in s2 {
                        // a rec set's `inherit x;` reads outside it; in a plain
                        // set that is the variable itself
                        let v = match v { Expr::Inherit(x, ip) if rec2 && !rec1 => Expr::Var(x, ip), v => v };
                        Self::insert_binding(&mut s1, n, p, v, what)?;
                    }
                    d1.extend(d2);
                    existing.2 = if rec1 { Expr::RecSet(s1, d1) } else { Self::set_of(s1, d1) };
                    Ok(())
                }
                (old, _) => {
                    existing.2 = match old {
                        Ok((s, d, true)) => Expr::RecSet(s, d),
                        Ok((s, d, false)) => Self::set_of(s, d),
                        Err(e) => e,
                    };
                    unsupported(pos, format!("`{name}` bound twice in one {what}"))
                }
            }
        } else {
            binds.push((name, pos, value));
            Ok(())
        }
    }

    /// The bindings of a `let` or an attribute set: `name = value;`, `inherit
    /// a b;` and `inherit (e) a b;`, and the computed ones, `${e} = value;`, in
    /// source order. In a `let`, `inherit a;` refers to the scope OUTSIDE the
    /// let.
    #[allow(clippy::type_complexity)]
    fn bindings(
        &self,
        node: &impl HasEntry,
        what: &str,
        is_let: bool,
    ) -> Result<(Vec<(String, Pos, Expr)>, Vec<(Expr, Pos, Expr)>), Unsupported> {
        let mut binds: Vec<(String, Pos, Expr)> = Vec::new();
        let mut dyns: Vec<(Expr, Pos, Expr)> = Vec::new();
        for entry in node.attrpath_values() {
            let Some(path) = entry.attrpath() else { return self.refuse(&entry, "binding without a name") };
            let keys = self.path_keys(&path)?;
            let Some(value) = entry.value() else { return self.refuse(&entry, "binding without a value") };
            let pos = self.pos(&path);
            // a.b.c = v  ==  a = { b = { c = v; }; }, merged with its siblings
            let mut value = self.expr(&value)?;
            let (first, rest) = keys.split_first().expect("non-empty attrpath");
            for k in rest.iter().rev() {
                value = match k {
                    Key::Name(n) => Expr::AttrSet(vec![(n.clone(), pos, value)]),
                    Key::Expr(e) => Expr::DynAttrSet(Vec::new(), vec![(e.clone(), pos, value)]),
                };
            }
            match first {
                Key::Name(n) => Self::insert_binding(&mut binds, n.clone(), pos, value, what)?,
                Key::Expr(e) => dyns.push((e.clone(), pos, value)),
            }
        }
        let mut push = |name: String, pos: Pos, e: Expr| -> Result<(), Unsupported> {
            Self::insert_binding(&mut binds, name, pos, e, what)
        };
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
                    Some(src) => Expr::Select(Box::new(src.clone()), vec![Key::Name(name.clone())], None),
                    None if is_let => Expr::Inherit(name.clone(), pos),
                    None => Expr::Var(name.clone(), pos),
                };
                push(name, pos, e)?;
            }
        }
        Ok((binds, dyns))
    }

    fn expr(&self, e: &ast::Expr) -> Result<Expr, Unsupported> {
        match e {
            ast::Expr::Literal(lit) => match lit.kind() {
                LiteralKind::Integer(i) => match i.value() {
                    Ok(v) => Ok(Expr::Int(v)),
                    Err(_) => self.refuse(lit, "integer literal out of range"),
                },
                LiteralKind::Float(f) => match float_literal(&f.to_string()) {
                    Some(v) => Ok(Expr::Float(v.to_bits())),
                    // Nix's parse error, for the whole file: refused like one.
                    None => self.refuse(lit, "invalid float"),
                },
                // A URI literal is a string (Nix 2.34, without the
                // no-url-literals experimental feature).
                LiteralKind::Uri(u) => Ok(Expr::Str(vec![StrPart::Lit(u.to_string())])),
            },
            ast::Expr::Ident(id) => match id.ident_token() {
                // `import` reaching here is `import` used as anything other
                // than the head of `import <path literal>`: passed around, it is
                // the primop, which reads its file at run time (D26).
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
                let (binds, dyns) = self.bindings(l, "let", true)?;
                if let Some((_, pos, _)) = dyns.first() {
                    return unsupported(*pos, "dynamic attributes not allowed in let");   // Nix's parse error
                }
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
                let (binds, dyns) = self.bindings(s, "attribute set", false)?;
                Ok(Self::set_of(binds, dyns))
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
                let names = self.path_keys(&path)?;
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
                let names = self.path_keys(&path)?;
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
            // <x>: Nix's parser makes it `__findFile __nixPath "x"`, both names
            // looked up in scope, so a let or a with may redefine either.
            ast::Expr::PathSearch(n) => {
                let text = n.to_string();
                let name = text.trim_start_matches('<').trim_end_matches('>').to_string();
                let pos = self.pos(n);
                Ok(Expr::Apply(
                    Box::new(Expr::Apply(
                        Box::new(Expr::Var("__findFile".into(), pos)),
                        Box::new(Expr::Var("__nixPath".into(), pos)),
                    )),
                    Box::new(Expr::Str(vec![StrPart::Lit(name)])),
                ))
            }
            ast::Expr::CurPos(n) => self.refuse(n, "__curPos"),
            ast::Expr::Root(n) => self.refuse(n, "nested root"),
            ast::Expr::Error(n) => self.refuse(n, "syntax error"),
        }
    }
}

/// A float literal's value, or None where Nix says "invalid float": Nix
/// parses with strtod and rejects the literal when errno is set, on overflow
/// and on underflow, which glibc reports when the result rounded to 53 bits
/// with an unbounded exponent is below the smallest normal 2^-1022. That is
/// every nonzero literal below 2^-1022 - 2^-1076, the midpoint under it, even
/// one that rounds to 2^-1022 itself (docs/FLOAT.md, measured: the midpoint
/// is valid, one unit in its last digit below is not). A zero is any zero:
/// 0.0e-400 is 0.
fn float_literal(text: &str) -> Option<f64> {
    let v: f64 = text.parse().ok()?;
    let (mant, exp) = match text.find(['e', 'E']) {
        Some(i) => (&text[..i], &text[i + 1..]),
        None => (text, "0"),
    };
    let (int, frac) = mant.split_once('.').unwrap_or((mant, ""));
    let all = format!("{int}{frac}");
    let digits = all.trim_start_matches('0');
    if digits.is_empty() {
        return Some(0.0);
    }
    if !v.is_finite() || v < f64::MIN_POSITIVE {
        return None;
    }
    if v > f64::MIN_POSITIVE {
        return Some(v);
    }
    // v is 2^-1022, so the literal lies within half a subnormal step of it:
    // compare it exactly with (2^54 - 1) * 5^1076 * 10^-1076, the midpoint.
    let mut limbs: Vec<u64> = vec![(1u64 << 54) - 1];          // base 10^9, little-endian
    let normal = |limbs: &mut Vec<u64>| {
        let mut c = 0;
        for l in limbs.iter_mut() {
            *l += c;
            c = *l / 1_000_000_000;
            *l %= 1_000_000_000;
        }
        while c > 0 {
            limbs.push(c % 1_000_000_000);
            c /= 1_000_000_000;
        }
    };
    normal(&mut limbs);
    for _ in 0..1076 {
        limbs.iter_mut().for_each(|l| *l *= 5);
        normal(&mut limbs);
    }
    let mut mid = limbs.last().unwrap().to_string();
    for l in limbs.iter().rev().skip(1) {
        mid.push_str(&format!("{l:09}"));
    }
    // Both are digits * 10^e; the number of digits plus e orders them, then
    // the digits themselves.
    let e: i64 = exp.parse().ok()?;
    let order = (digits.len() as i64 + e - frac.len() as i64).cmp(&(mid.len() as i64 - 1076));
    let n = digits.len().max(mid.len());
    let below = order.is_lt()
        || (order.is_eq() && format!("{digits:0<n$}") < format!("{mid:0<n$}"));
    if below { None } else { Some(v) }
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
            Expr::RecSet(bs, ds) => Expr::RecSet(
                bs.into_iter().map(|(n, _, e)| (n, Pos { line: 1, col: 0 }, strip(e))).collect(),
                ds.into_iter().map(|(n, _, e)| (strip(n), Pos { line: 1, col: 0 }, strip(e))).collect(),
            ),
            Expr::DynAttrSet(bs, ds) => Expr::DynAttrSet(
                bs.into_iter().map(|(n, _, e)| (n, Pos { line: 1, col: 0 }, strip(e))).collect(),
                ds.into_iter().map(|(n, _, e)| (strip(n), Pos { line: 1, col: 0 }, strip(e))).collect(),
            ),
            Expr::Select(e, path, d) => {
                Expr::Select(Box::new(strip(*e)), strip_keys(path), d.map(|d| Box::new(strip(*d))))
            }
            Expr::HasAttr(e, path) => Expr::HasAttr(Box::new(strip(*e)), strip_keys(path)),
            Expr::Update(a, b) => Expr::Update(Box::new(strip(*a)), Box::new(strip(*b))),
            Expr::Concat(a, b) => Expr::Concat(Box::new(strip(*a)), Box::new(strip(*b))),
            Expr::Inherit(n, _) => Expr::Inherit(n, Pos { line: 1, col: 0 }),
            Expr::Seq(a, b) => Expr::Seq(Box::new(strip(*a)), Box::new(strip(*b))),
            e => e,
        }
    }
    fn strip_keys(ks: Vec<Key>) -> Vec<Key> {
        ks.into_iter().map(|k| match k { Key::Expr(e) => Key::Expr(strip(e)), k => k }).collect()
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
    fn float_literals_are_strtods() {
        // Each answer measured with nix-instantiate --eval (Nix 2.34.9).
        let below = "0.0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000222507385850720125957382125702";
        let above = "0.0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000222507385850720125957382125703";
        let cases: [(&str, Option<u64>); 11] = [
            ("1.5", Some(0x3FF8000000000000)),
            ("0.0e-400", Some(0)),
            ("0.000e999", Some(0)),
            ("1.0e-400", None),
            ("4.9e-324", None),
            ("2.2250738585072011e-308", None),
            ("2.2250738585072012e-308", None),     // rounds to 2^-1022, still invalid
            ("2.2250738585072013e-308", Some(0x0010000000000000)),
            ("1.7976931348623158e308", Some(0x7FEFFFFFFFFFFFFF)),
            ("1.7976931348623159e308", None),
            (".5", Some(0x3FE0000000000000)),
        ];
        for (text, want) in cases {
            assert_eq!(float_literal(text).map(f64::to_bits), want, "{text}");
        }
        // The midpoint under 2^-1022, to 30 digits either side of it.
        assert_eq!(float_literal(below), None);
        assert_eq!(float_literal(above).map(f64::to_bits), Some(0x0010000000000000));
    }

    #[test]
    fn a_literal_in_dollar_braces_is_a_static_name() {
        // Nix's parser: `${"a"}`, `${("a")}` and `${''a''}` are the name `a`;
        // `"${"a"}"` is a concatenation, computed (measured with rec sets).
        for src in ["{ ${\"a\"} = 1; }", "{ ${(\"a\")} = 1; }", "{ ${''a''} = 1; }", "{ \"a\" = 1; }"] {
            assert!(matches!(p(src), Expr::AttrSet(_)), "{src}: {:?}", p(src));
        }
        for src in ["{ \"${\"a\"}\" = 1; }", "{ ${a} = 1; }", "{ \"x${a}\" = 1; }"] {
            assert!(matches!(p(src), Expr::DynAttrSet(..)), "{src}: {:?}", p(src));
        }
        // a computed name inside a static one merges with its siblings
        assert!(matches!(p("{ a.${b} = 1; a.c = 2; }"), Expr::AttrSet(ref s) if s.len() == 1
            && matches!(s[0].2, Expr::DynAttrSet(ref st, ref d) if st.len() == 1 && d.len() == 1)));
        // ...and computed names are never merged with each other
        assert!(matches!(p("{ ${a}.b = 1; ${a}.c = 2; }"), Expr::DynAttrSet(ref st, ref d) if st.is_empty() && d.len() == 2));
        assert!(matches!(p("x.${a}.b"), Expr::Select(_, ref k, None) if matches!(k[0], Key::Expr(_)) && k[1] == Key::Name("b".into())));
    }

    #[test]
    fn imports_not_resolved_here_are_the_primop() {
        // a computed import, and import passed around: the primop, applied
        // at run time (D26), never a guess at the file
        assert!(matches!(p("import (f 1)"), Expr::Apply(ref f, _) if matches!(**f, Expr::Var(ref n, _) if n == "import")));
        assert!(matches!(p("[ import ]"), Expr::List(ref l) if matches!(l[0], Expr::Var(ref n, _) if n == "import")));
    }

    #[test]
    fn search_paths_are_find_file_calls() {
        // <x> is Nix's `__findFile __nixPath "x"`, both names looked up in scope
        match p("<nixpkgs/lib>") {
            Expr::Apply(f, a) => {
                assert!(matches!(*f, Expr::Apply(ref g, ref np)
                    if matches!(**g, Expr::Var(ref n, _) if n == "__findFile")
                    && matches!(**np, Expr::Var(ref n, _) if n == "__nixPath")));
                assert!(matches!(*a, Expr::Str(ref parts) if matches!(parts[..], [StrPart::Lit(ref t)] if t == "nixpkgs/lib")));
            }
            e => panic!("{e:?}"),
        }
    }

    #[test]
    fn refuses_by_feature_name() {
        let cases = [
            ("let ${a} = 1; in 1", "dynamic attributes not allowed in let"),
            ("let \"${a}\" = 1; in 1", "dynamic attributes not allowed in let"),
            ("{ inherit ${a}; }", "dynamic attributes not allowed in inherit"),
            ("./p/${a}", "path antiquotation"),
            ("1.0e309", "invalid float"),
            ("2.2250738585072011e-308", "invalid float"),
            ("let x = 1; x = 2; in x", "`x` bound twice in one let"),
            ("{ a = 1; inherit a; }", "`a` bound twice in one attribute set"),
            // Imports the generator will not guess at (docs/BYTECODE.md).
            ("import ./lib.nix", "import with no file to resolve against"),
            ("let import = 1; in 2", "import is shadowed"),
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
        let err = parse("let\n  x = 1;\n  in __curPos").unwrap_err();   // any refused construct
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
            Expr::RecSet(
                vec![("a".into(), Pos { line: 1, col: 0 }, Expr::Int(1)), ("b".into(), Pos { line: 1, col: 0 }, *var("a"))],
                vec![]
            )
        );
        // merged: the first one's rec-ness (Nix's parser)
        assert!(matches!(p("{ s = rec { a = 1; }; s = { b = a; }; }"), Expr::AttrSet(ref v) if matches!(v[0].2, Expr::RecSet(ref b, _) if b.len() == 2)));
        assert!(matches!(p("{ s = { a = 1; }; s = rec { b = a; }; }"), Expr::AttrSet(ref v) if matches!(v[0].2, Expr::AttrSet(ref b) if b.len() == 2)));
        assert_eq!(
            p("let { body = 1; }"),
            Expr::Select(
                Box::new(Expr::RecSet(vec![("body".into(), Pos { line: 1, col: 0 }, Expr::Int(1))], vec![])),
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
