//! nbc -- test-only .nbc fixture generator for the gs-os-nix 65816 VM.
//!
//!   nbc <in.nix> -o <out.nbc>     compile
//!   nbc --dump <file.nbc>         disassemble
//!
//! Exit status 2 means "outside the supported subset" -- the scoreboard counts
//! that as UNSUPPORTED rather than as a VM failure. Per docs/DECISIONS.md D7 this
//! tool is a fixture generator, not a runtime component: it contains no hashing
//! and no store-path code, and checks.fixture-purity keeps it that way.

mod compiler;
mod lexer;
mod nbc;
mod opcodes;
mod parser;

use std::process::ExitCode;

fn usage() -> ExitCode {
    eprintln!("usage: nbc <in.nix> -o <out.nbc>\n       nbc --dump <file.nbc>");
    ExitCode::from(1)
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match args.as_slice() {
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
            let src = match std::fs::read_to_string(input) {
                Ok(s) => s,
                Err(e) => {
                    eprintln!("nbc: {input}: {e}");
                    return ExitCode::from(1);
                }
            };
            let compiled = lexer::lex(&src)
                .and_then(|t| parser::parse(&t))
                .and_then(|ast| compiler::compile(&ast));
            match compiled {
                Ok(chunks) => {
                    if let Err(e) = std::fs::write(output, nbc::write(&chunks)) {
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
