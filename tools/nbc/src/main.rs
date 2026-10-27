//! nbc -- test-only .nbc fixture generator for the gs-os-nix 65816 VM.
//!
//!   nbc <in.nix> -o <out.nbc>     compile
//!   nbc --dump <file.nbc>         disassemble
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
