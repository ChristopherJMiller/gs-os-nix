//! nbc -- test-only .nbc fixture generator for the gs-os-nix 65816 VM.
//!
//!   nbc <in.nix> -o <out.nbc>     compile
//!   nbc --dump <file.nbc>         disassemble
//!   nbc --tokens <in.nix>         rnix's tokens: the on-target lexer's oracle
//!                                 (docs/COMPILER.md "Stage 1")
//!   nbc --cst <in.nix>            rnix's syntax tree: the on-target parser's
//!   nbc --ast <in.nix>            the lowered AST: the on-target lowering's
//!
//! Exit status 2 means "outside the supported subset" -- the scoreboard counts
//! that as UNSUPPORTED rather than as a VM failure. Per docs/DECISIONS.md D7 this
//! tool is a fixture generator, not a runtime component: it contains no hashing
//! and no store-path code, and checks.fixture-purity keeps it that way.

mod ast;
mod compiler;
mod front;
mod nbc;
mod opcodes;
mod sexp;

use std::process::ExitCode;

fn usage() -> ExitCode {
    eprintln!("usage: nbc <in.nix> -o <out.nbc>\n       nbc --dump <file.nbc>\n       nbc --tokens <in.nix>\n       nbc --cst <in.nix>\n       nbc --ast <in.nix>");
    ExitCode::from(1)
}

/// The file as text: not UTF-8 is refused as a compile is (exit 2), since
/// rnix takes a `&str`.
fn read_text(path: &str) -> Result<String, ExitCode> {
    match std::fs::read(path).map(String::from_utf8) {
        Ok(Ok(s)) => Ok(s),
        Ok(Err(_)) => {
            eprintln!("nbc: {path}: not UTF-8");
            Err(ExitCode::from(2))
        }
        Err(e) => {
            eprintln!("nbc: {path}: {e}");
            Err(ExitCode::from(1))
        }
    }
}

/// One line a token: its SyntaxKind's number and name, its byte offset and
/// length -- what `a2min --lex` prints for the VM's lexer, but the name.
fn tokens(path: &str) -> ExitCode {
    let src = match read_text(path) {
        Ok(s) => s,
        Err(code) => return code,
    };
    let mut at = 0usize;
    let mut out = String::new();
    for (kind, text) in rnix::tokenize(&src) {
        out.push_str(&format!("{} {:?} {} {}\n", kind as u16, kind, at, text.len()));
        at += text.len();
    }
    print!("{out}");
    ExitCode::SUCCESS
}

/// rnix's syntax tree in preorder, one line a node or token: depth, kind,
/// start, length -- what `a2min --cst` prints for the VM's parser. Any error
/// rnix reports is a refusal (exit 2), as a compile's is, and nothing else
/// is printed: the VM's parser stops at the first one (COMPILER.md).
fn cst(path: &str) -> ExitCode {
    let src = match read_text(path) {
        Ok(s) => s,
        Err(code) => return code,
    };
    let parsed = rnix::Root::parse(&src);
    if !parsed.errors().is_empty() {
        eprintln!("nbc: {path}: {}", parsed.errors()[0]);
        return ExitCode::from(2);
    }
    fn walk(n: &rnix::SyntaxNode, depth: usize, out: &mut String) {
        let r = n.text_range();
        out.push_str(&format!("{} {} {} {}\n", depth, n.kind() as u16, u32::from(r.start()), u32::from(r.len())));
        for c in n.children_with_tokens() {
            match c {
                rnix::NodeOrToken::Node(n) => walk(&n, depth + 1, out),
                rnix::NodeOrToken::Token(t) => {
                    let r = t.text_range();
                    out.push_str(&format!("{} {} {} {}\n", depth + 1, t.kind() as u16, u32::from(r.start()), u32::from(r.len())));
                }
            }
        }
    }
    let mut out = String::new();
    walk(&parsed.syntax(), 0, &mut out);
    print!("{out}");
    ExitCode::SUCCESS
}

/// The lowered AST as an S-expression (sexp.rs): what `a2min --ast` prints
/// for the on-target lowering. A refusal, of any kind, is exit 2.
fn ast_dump(path: &str) -> ExitCode {
    let p = std::path::Path::new(path);
    match front::parse_file(p) {
        Ok(e) => {
            let here = p.canonicalize().unwrap_or_else(|_| p.to_path_buf());
            let base = here.parent().map(|d| d.to_string_lossy().into_owned()).unwrap_or_default();
            println!("{}", sexp::sexp(&e, &base));
            ExitCode::SUCCESS
        }
        Err(e) => {
            eprintln!("nbc: {path}:{e}");
            ExitCode::from(2)
        }
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match args.as_slice() {
        [flag, path] if flag == "--tokens" => tokens(path),
        [flag, path] if flag == "--cst" => cst(path),
        [flag, path] if flag == "--ast" => ast_dump(path),
        [flag, path] if flag == "--dump" => {
            let bytes = match std::fs::read(path) {
                Ok(b) => b,
                Err(e) => {
                    eprintln!("nbc: {path}: {e}");
                    return ExitCode::from(1);
                }
            };
            match nbc::read(&bytes) {
                Ok(img) => {
                    print!("{}", nbc::disassemble(&img));
                    ExitCode::SUCCESS
                }
                Err(e) => {
                    eprintln!("nbc: {path}: {e}");
                    ExitCode::from(1)
                }
            }
        }
        [input, flag, output] if flag == "-o" => {
            // parse_file, not parse: `import ./lib.nix` is resolved relative to
            // the file being compiled (docs/BYTECODE.md "Imports").
            let path = std::path::Path::new(input);
            let compiled = front::parse_file(path).and_then(|ast| compiler::compile_file(&ast, Some(path)));
            match compiled {
                Ok(program) => {
                    if let Err(e) = std::fs::write(output, nbc::write_program(&program)) {
                        eprintln!("nbc: {output}: {e}");
                        return ExitCode::from(1);
                    }
                    ExitCode::SUCCESS
                }
                Err(e) => {
                    eprintln!("nbc: {input}:{e}");
                    ExitCode::from(2)
                }
            }
        }
        _ => usage(),
    }
}
