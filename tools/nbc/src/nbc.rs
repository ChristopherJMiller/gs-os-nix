//! The .nbc container: docs/BYTECODE.md "File layout". Writer, reader and
//! disassembler. The reader exists for `nbc --dump` and for the round-trip test;
//! the VM has its own loader in 65816 assembly.

use crate::compiler::{Chunk, Const};
use crate::opcodes::*;

pub const MAGIC: &[u8; 3] = b"NBC";
pub const VERSION: u8 = 1;
pub const HEADER_LEN: usize = 16;

pub fn write(chunks: &[Chunk]) -> Vec<u8> {
    // Serialise the chunks first so the table can be filled in.
    let bodies: Vec<Vec<u8>> = chunks.iter().map(write_chunk).collect();
    let table_len = 4 * chunks.len();
    let mut out = Vec::new();
    out.extend_from_slice(MAGIC);
    out.push(VERSION);
    out.extend_from_slice(&(chunks.len() as u16).to_le_bytes());
    out.extend_from_slice(&0u16.to_le_bytes()); // entry chunk
    let total: usize = HEADER_LEN + table_len + bodies.iter().map(Vec::len).sum::<usize>();
    out.extend_from_slice(&(total as u32).to_le_bytes());
    out.extend_from_slice(&0u32.to_le_bytes()); // reserved
    let mut off = HEADER_LEN + table_len;
    for b in &bodies {
        out.extend_from_slice(&(off as u32).to_le_bytes());
        off += b.len();
    }
    for b in bodies {
        out.extend_from_slice(&b);
    }
    debug_assert_eq!(out.len(), total);
    out
}

fn write_chunk(ch: &Chunk) -> Vec<u8> {
    let mut out = Vec::new();
    out.extend_from_slice(&(ch.consts.len() as u16).to_le_bytes());
    out.extend_from_slice(&(ch.code.len() as u16).to_le_bytes());
    for k in &ch.consts {
        match k {
            Const::Int(v) => {
                out.push(K_INT);
                out.extend_from_slice(&v.to_le_bytes());
            }
            Const::Str(s) => {
                out.push(K_STRING);
                out.extend_from_slice(&(s.len() as u16).to_le_bytes());
                out.extend_from_slice(s.as_bytes());
            }
        }
    }
    out.extend_from_slice(&ch.code);
    out
}

#[derive(Debug)]
pub struct Image {
    pub entry: u16,
    pub chunks: Vec<Chunk>,
}

fn u16_at(b: &[u8], at: usize) -> Result<u16, String> {
    b.get(at..at + 2)
        .map(|s| u16::from_le_bytes([s[0], s[1]]))
        .ok_or_else(|| format!("truncated at {at}"))
}
fn u32_at(b: &[u8], at: usize) -> Result<u32, String> {
    b.get(at..at + 4)
        .map(|s| u32::from_le_bytes([s[0], s[1], s[2], s[3]]))
        .ok_or_else(|| format!("truncated at {at}"))
}

pub fn read(b: &[u8]) -> Result<Image, String> {
    if b.len() < HEADER_LEN || &b[0..3] != MAGIC {
        return Err("not an .nbc file".into());
    }
    if b[3] != VERSION {
        return Err(format!("version {} (want {VERSION})", b[3]));
    }
    let n = u16_at(b, 4)? as usize;
    let entry = u16_at(b, 6)?;
    let total = u32_at(b, 8)? as usize;
    if total != b.len() {
        return Err(format!("total_size {total} but file is {} bytes", b.len()));
    }
    let mut chunks = Vec::with_capacity(n);
    for i in 0..n {
        let off = u32_at(b, HEADER_LEN + 4 * i)? as usize;
        let nconst = u16_at(b, off)? as usize;
        let code_len = u16_at(b, off + 2)? as usize;
        let mut p = off + 4;
        let mut consts = Vec::with_capacity(nconst);
        for _ in 0..nconst {
            match b.get(p) {
                Some(&K_INT) => {
                    let s = b.get(p + 1..p + 9).ok_or("truncated constant")?;
                    consts.push(Const::Int(i64::from_le_bytes(s.try_into().unwrap())));
                    p += 9;
                }
                Some(&K_STRING) => {
                    let len = u16_at(b, p + 1)? as usize;
                    let s = b.get(p + 3..p + 3 + len).ok_or("truncated string constant")?;
                    consts.push(Const::Str(String::from_utf8_lossy(s).into_owned()));
                    p += 3 + len;
                }
                Some(k) => return Err(format!("unknown constant kind {k:#04x}")),
                None => return Err("truncated constant".into()),
            }
        }
        let code = b.get(p..p + code_len).ok_or("truncated code")?.to_vec();
        chunks.push(Chunk { consts, code });
    }
    Ok(Image { entry, chunks })
}

pub fn disassemble(img: &Image) -> String {
    let mut s = String::new();
    s.push_str(&format!("entry chunk {}\n", img.entry));
    for (ci, ch) in img.chunks.iter().enumerate() {
        s.push_str(&format!("chunk {ci}: {} constants, {} bytes\n", ch.consts.len(), ch.code.len()));
        for (ki, k) in ch.consts.iter().enumerate() {
            match k {
                Const::Int(v) => s.push_str(&format!("  k{ki} = INT {v}\n")),
                Const::Str(v) => s.push_str(&format!("  k{ki} = STRING {v:?}\n")),
            }
        }
        let mut pc = 0;
        while pc < ch.code.len() {
            let op = ch.code[pc];
            match describe(op) {
                Some((name, width)) => {
                    let operands = &ch.code[pc + 1..(pc + 1 + width).min(ch.code.len())];
                    let text = match (op, operands) {
                        (OP_JMP | OP_JMP_IF_FALSE | OP_JMP_IF_TRUE, [lo, hi]) => {
                            let rel = i16::from_le_bytes([*lo, *hi]);
                            format!("{name} {rel:+} (-> {})", (pc as i64 + 3 + rel as i64))
                        }
                        (OP_ENV_GET, [d0, d1, i0, i1]) => {
                            format!("{name} depth {} index {}", u16::from_le_bytes([*d0, *d1]), u16::from_le_bytes([*i0, *i1]))
                        }
                        (OP_PRIM, [id]) => {
                            format!("{name} {}", primop_name(*id).unwrap_or("?"))
                        }
                        (_, [lo, hi]) => format!("{name} {}", u16::from_le_bytes([*lo, *hi])),
                        _ => name.to_string(),
                    };
                    s.push_str(&format!("  {pc:04x}  {text}\n"));
                    pc += 1 + width;
                }
                None => {
                    s.push_str(&format!("  {pc:04x}  ?? {op:#04x}\n"));
                    pc += 1;
                }
            }
        }
    }
    s
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::compiler::compile;
    use crate::front::parse;

    #[test]
    fn round_trips_and_lays_out_the_header() {
        let chunks = compile(&parse("let x = 1 + 2; in x").unwrap()).unwrap();
        let bytes = write(&chunks);
        assert_eq!(&bytes[0..4], b"NBC\x01");
        assert_eq!(u16::from_le_bytes([bytes[4], bytes[5]]), 2);
        assert_eq!(u32::from_le_bytes([bytes[8], bytes[9], bytes[10], bytes[11]]) as usize, bytes.len());
        // chunk 0 starts right after the 2-entry table
        assert_eq!(u32::from_le_bytes([bytes[16], bytes[17], bytes[18], bytes[19]]), 24);
        let img = read(&bytes).unwrap();
        assert_eq!(img.chunks, chunks);
        assert!(disassemble(&img).contains("THUNK 1"));

        let chunks = compile(&parse("{ a = \"x\\ny\"; } // builtins.length [ ]").unwrap()).unwrap();
        let img = read(&write(&chunks)).unwrap();
        assert_eq!(img.chunks, chunks);
        let dis = disassemble(&img);
        assert!(dis.contains("STRING \"x\\ny\""), "{dis}");
        assert!(dis.contains("PRIM length"), "{dis}");
    }

    #[test]
    fn rejects_corruption() {
        let chunks = compile(&parse("1").unwrap()).unwrap();
        let mut bytes = write(&chunks);
        bytes[3] = 9;
        assert!(read(&bytes).is_err());
        let mut bytes = write(&chunks);
        bytes.push(0);
        assert!(read(&bytes).is_err());
    }
}
