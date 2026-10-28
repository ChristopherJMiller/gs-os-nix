//! The lowered AST as an S-expression: what `nbc --ast` prints and
//! `a2min --ast` prints for the on-target lowering, so the two can be diffed
//! (docs/COMPILER.md "Stage 2b"). A node is a parenthesis; a name or a
//! string part is quoted, every byte outside printable ASCII, `"` and `\`
//! written `\xNN`; a position is `L:C`; an absent part is `nil`. An import's
//! key is printed relative to `base`, the compiled file's directory, so a
//! host path and a GS/OS pathname compare.

use crate::ast::{BinOp, Expr, Key, Param, Pos, StrPart};

pub fn sexp(e: &Expr, base: &str) -> String {
    let mut out = String::new();
    expr(e, base, &mut out);
    out
}

/// `key` relative to the directory `base`, both absolute: the components
/// they share dropped, a `..` for each of base's left.
fn relative(key: &str, base: &str) -> String {
    let k: Vec<&str> = key.split('/').filter(|c| !c.is_empty()).collect();
    let b: Vec<&str> = base.split('/').filter(|c| !c.is_empty()).collect();
    let common = k.iter().zip(&b).take_while(|(x, y)| x == y).count();
    let mut parts: Vec<&str> = vec![".."; b.len() - common];
    parts.extend(&k[common..]);
    parts.join("/")
}

fn q(s: &str, out: &mut String) {
    out.push('"');
    for &b in s.as_bytes() {
        if (0x20..0x7F).contains(&b) && b != b'"' && b != b'\\' {
            out.push(b as char);
        } else {
            out.push_str(&format!("\\x{b:02X}"));
        }
    }
    out.push('"');
}

fn pos(p: &Pos, out: &mut String) {
    out.push_str(&format!("{}:{}", p.line, p.col));
}

fn binds(bs: &[(String, Pos, Expr)], base: &str, out: &mut String) {
    out.push('(');
    for (i, (n, p, e)) in bs.iter().enumerate() {
        if i > 0 {
            out.push(' ');
        }
        out.push('(');
        q(n, out);
        out.push(' ');
        pos(p, out);
        out.push(' ');
        expr(e, base, out);
        out.push(')');
    }
    out.push(')');
}

fn dyns(ds: &[(Expr, Pos, Expr)], base: &str, out: &mut String) {
    out.push('(');
    for (i, (k, p, e)) in ds.iter().enumerate() {
        if i > 0 {
            out.push(' ');
        }
        out.push('(');
        expr(k, base, out);
        out.push(' ');
        pos(p, out);
        out.push(' ');
        expr(e, base, out);
        out.push(')');
    }
    out.push(')');
}

fn parts(head: &str, ps: &[StrPart], base: &str, out: &mut String) {
    out.push('(');
    out.push_str(head);
    for p in ps {
        out.push(' ');
        match p {
            StrPart::Lit(s) => q(s, out),
            StrPart::Interp(e) => {
                out.push_str("(interp ");
                expr(e, base, out);
                out.push(')');
            }
        }
    }
    out.push(')');
}

fn keys(ks: &[Key], base: &str, out: &mut String) {
    out.push('(');
    for (i, k) in ks.iter().enumerate() {
        if i > 0 {
            out.push(' ');
        }
        match k {
            Key::Name(n) => q(n, out),
            Key::Expr(e) => {
                out.push_str("(expr ");
                expr(e, base, out);
                out.push(')');
            }
        }
    }
    out.push(')');
}

fn opt(e: &Option<Box<Expr>>, base: &str, out: &mut String) {
    match e {
        Some(e) => expr(e, base, out),
        None => out.push_str("nil"),
    }
}

fn node(head: &str, kids: &[&Expr], base: &str, out: &mut String) {
    out.push('(');
    out.push_str(head);
    for k in kids {
        out.push(' ');
        expr(k, base, out);
    }
    out.push(')');
}

fn expr(e: &Expr, base: &str, out: &mut String) {
    match e {
        Expr::Int(v) => out.push_str(&format!("(int {v})")),
        Expr::Float(b) => out.push_str(&format!("(float {b:016X})")),
        Expr::Var(n, p) => {
            out.push_str("(var ");
            q(n, out);
            out.push(' ');
            pos(p, out);
            out.push(')');
        }
        Expr::Neg(a) => node("neg", &[a], base, out),
        Expr::Not(a) => node("not", &[a], base, out),
        Expr::Bin(op, a, b) => {
            let name = match op {
                BinOp::Add => "add",
                BinOp::Sub => "sub",
                BinOp::Mul => "mul",
                BinOp::Div => "div",
                BinOp::Eq => "eq",
                BinOp::Ne => "ne",
                BinOp::Lt => "lt",
                BinOp::Le => "le",
                BinOp::Gt => "gt",
                BinOp::Ge => "ge",
            };
            node(name, &[a, b], base, out)
        }
        Expr::And(a, b) => node("and", &[a, b], base, out),
        Expr::Or(a, b) => node("or", &[a, b], base, out),
        Expr::Implies(a, b) => node("implies", &[a, b], base, out),
        Expr::If(c, t, f) => node("if", &[c, t, f], base, out),
        Expr::Let(bs, body) => {
            out.push_str("(let ");
            binds(bs, base, out);
            out.push(' ');
            expr(body, base, out);
            out.push(')');
        }
        Expr::Lambda(param, p, body) => {
            out.push_str("(lambda ");
            match param {
                Param::Ident(n) => {
                    out.push_str("(ident ");
                    q(n, out);
                    out.push(')');
                }
                Param::Formals { entries, ellipsis, bind } => {
                    out.push_str("(formals (");
                    for (i, (n, ep, d)) in entries.iter().enumerate() {
                        if i > 0 {
                            out.push(' ');
                        }
                        out.push('(');
                        q(n, out);
                        out.push(' ');
                        pos(ep, out);
                        out.push(' ');
                        match d {
                            Some(d) => expr(d, base, out),
                            None => out.push_str("nil"),
                        }
                        out.push(')');
                    }
                    out.push_str(if *ellipsis { ") t " } else { ") nil " });
                    match bind {
                        Some(b) => q(b, out),
                        None => out.push_str("nil"),
                    }
                    out.push(')');
                }
            }
            out.push(' ');
            pos(p, out);
            out.push(' ');
            expr(body, base, out);
            out.push(')');
        }
        Expr::With(a, b) => node("with", &[a, b], base, out),
        Expr::Apply(f, a) => node("apply", &[f, a], base, out),
        Expr::Assert(c, b) => node("assert", &[c, b], base, out),
        Expr::Str(ps) => parts("str", ps, base, out),
        Expr::PathInterp(ps) => parts("pathi", ps, base, out),
        Expr::Path(s) => {
            out.push_str("(path ");
            q(s, out);
            out.push(')');
        }
        Expr::List(items) => {
            out.push_str("(list");
            for it in items {
                out.push(' ');
                expr(it, base, out);
            }
            out.push(')');
        }
        Expr::AttrSet(bs) => {
            out.push_str("(attrs ");
            binds(bs, base, out);
            out.push(')');
        }
        Expr::DynAttrSet(bs, ds) => {
            out.push_str("(dynattrs ");
            binds(bs, base, out);
            out.push(' ');
            dyns(ds, base, out);
            out.push(')');
        }
        Expr::RecSet(bs, ds) => {
            out.push_str("(recset ");
            binds(bs, base, out);
            out.push(' ');
            dyns(ds, base, out);
            out.push(')');
        }
        Expr::Select(e, ks, d) => {
            out.push_str("(select ");
            expr(e, base, out);
            out.push(' ');
            keys(ks, base, out);
            out.push(' ');
            opt(d, base, out);
            out.push(')');
        }
        Expr::HasAttr(e, ks) => {
            out.push_str("(hasattr ");
            expr(e, base, out);
            out.push(' ');
            keys(ks, base, out);
            out.push(')');
        }
        Expr::Update(a, b) => node("update", &[a, b], base, out),
        Expr::Concat(a, b) => node("concat", &[a, b], base, out),
        Expr::Inherit(n, p) => {
            out.push_str("(inherit ");
            q(n, out);
            out.push(' ');
            pos(p, out);
            out.push(')');
        }
        Expr::Seq(a, b) => node("seq", &[a, b], base, out),
        Expr::Import(key, body) => {
            out.push_str("(import ");
            q(&relative(key, base), out);
            out.push(' ');
            expr(body, base, out);
            out.push(')');
        }
    }
}
