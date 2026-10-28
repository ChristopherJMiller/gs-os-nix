//! nbc -- test-only .nbc fixture generator for the gs-os-nix 65816 VM.
//!
//!   nbc <in.nix> -o <out.nbc>     compile
//!   nbc --dump <file.nbc>         disassemble
//!   nbc --tokens <in.nix>         rnix's tokens: the on-target lexer's oracle
//!                                 (docs/COMPILER.md "Stage 1")
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

use std::process::ExitCode;

fn usage() -> ExitCode {
    eprintln!("usage: nbc <in.nix> -o <out.nbc>\n       nbc --dump <file.nbc>\n       nbc --tokens <in.nix>");
    ExitCode::from(1)
}

/// One line a token: its SyntaxKind's number and name, its byte offset and
/// length -- what `a2min --lex` prints for the VM's lexer, but the name. Not
/// UTF-8 is refused as a compile is (exit 2): rnix takes a `&str`.
fn tokens(path: &str) -> ExitCode {
    let src = match std::fs::read(path).map(String::from_utf8) {
        Ok(Ok(s)) => s,
        Ok(Err(_)) => {
            eprintln!("nbc: {path}: not UTF-8");
            return ExitCode::from(2);
        }
        Err(e) => {
            eprintln!("nbc: {path}: {e}");
            return ExitCode::from(1);
        }
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

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match args.as_slice() {
        [flag, path] if flag == "--tokens" => tokens(path),
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
