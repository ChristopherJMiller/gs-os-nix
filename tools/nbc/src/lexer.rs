//! Tokens for the slice-1 subset of Nix: integers, identifiers, the arithmetic and
//! boolean operators, `let`/`in`/`if`/`then`/`else`, parentheses, comments.
//!
//! Anything outside the subset is reported as `Unsupported`, never guessed at. The
//! scoreboard counts those separately from VM failures, so an honest "I can't
//! compile this yet" is worth more than a plausible wrong fixture.

use std::fmt;

#[derive(Debug, Clone, PartialEq)]
pub enum Tok {
    Int(i64),
    Ident(String),
    Let,
    In,
    If,
    Then,
    Else,
    Plus,
    Minus,
    Star,
    Slash,
    EqEq,
    NotEq,
    Lt,
    Le,
    Gt,
    Ge,
    AndAnd,
    OrOr,
    Implies,
    Bang,
    LParen,
    RParen,
    Assign,
    Semi,
    Eof,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Pos {
    pub line: usize,
    pub col: usize,
}

impl fmt::Display for Pos {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}:{}", self.line, self.col)
    }
}

/// The generator's only error type. Everything it cannot handle -- unknown syntax,
/// a feature outside the current slice, an unbound name -- is "unsupported".
#[derive(Debug, Clone, PartialEq)]
pub struct Unsupported {
    pub pos: Pos,
    pub what: String,
}

impl fmt::Display for Unsupported {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}: unsupported: {}", self.pos, self.what)
    }
}

pub fn unsupported<T>(pos: Pos, what: impl Into<String>) -> Result<T, Unsupported> {
    Err(Unsupported { pos, what: what.into() })
}

fn is_ident_start(c: char) -> bool {
    c.is_ascii_alphabetic() || c == '_'
}

fn is_ident_char(c: char) -> bool {
    c.is_ascii_alphanumeric() || c == '_' || c == '\'' || c == '-'
}

pub fn lex(src: &str) -> Result<Vec<(Tok, Pos)>, Unsupported> {
    let chars: Vec<char> = src.chars().collect();
    let mut out = Vec::new();
    let mut i = 0;
    let mut line = 1;
    let mut col = 1;

    fn advance(i: &mut usize, line: &mut usize, col: &mut usize, chars: &[char]) {
        let c = chars[*i];
        *i += 1;
        if c == '\n' {
            *line += 1;
            *col = 1;
        } else {
            *col += 1;
        }
    }

    while i < chars.len() {
        let c = chars[i];
        let pos = Pos { line, col };

        if c.is_whitespace() {
            advance(&mut i, &mut line, &mut col, &chars);
            continue;
        }
        if c == '#' {
            while i < chars.len() && chars[i] != '\n' {
                advance(&mut i, &mut line, &mut col, &chars);
            }
            continue;
        }
        if c == '/' && chars.get(i + 1) == Some(&'*') {
            advance(&mut i, &mut line, &mut col, &chars);
            advance(&mut i, &mut line, &mut col, &chars);
            loop {
                if i >= chars.len() {
                    return unsupported(pos, "unterminated comment");
                }
                if chars[i] == '*' && chars.get(i + 1) == Some(&'/') {
                    advance(&mut i, &mut line, &mut col, &chars);
                    advance(&mut i, &mut line, &mut col, &chars);
                    break;
                }
                advance(&mut i, &mut line, &mut col, &chars);
            }
            continue;
        }

        if c.is_ascii_digit() {
            let start = i;
            while i < chars.len() && chars[i].is_ascii_digit() {
                advance(&mut i, &mut line, &mut col, &chars);
            }
            // A dot or exponent after digits is a float, which is not in this slice.
            if i < chars.len() && (chars[i] == '.' || chars[i] == 'e' || chars[i] == 'E') {
                return unsupported(pos, "float literal");
            }
            let text: String = chars[start..i].iter().collect();
            let v: i64 = match text.parse() {
                Ok(v) => v,
                Err(_) => return unsupported(pos, format!("integer literal out of range: {text}")),
            };
            out.push((Tok::Int(v), pos));
            continue;
        }

        if is_ident_start(c) {
            let start = i;
            while i < chars.len() && is_ident_char(chars[i]) {
                advance(&mut i, &mut line, &mut col, &chars);
            }
            let text: String = chars[start..i].iter().collect();
            let tok = match text.as_str() {
                "let" => Tok::Let,
                "in" => Tok::In,
                "if" => Tok::If,
                "then" => Tok::Then,
                "else" => Tok::Else,
                "with" | "rec" | "inherit" | "assert" | "or" => {
                    return unsupported(pos, format!("keyword `{text}`"))
                }
                _ => Tok::Ident(text),
            };
            out.push((tok, pos));
            continue;
        }

        let two: String = chars[i..chars.len().min(i + 2)].iter().collect();
        let (tok, len) = match two.as_str() {
            "==" => (Tok::EqEq, 2),
            "!=" => (Tok::NotEq, 2),
            "<=" => (Tok::Le, 2),
            ">=" => (Tok::Ge, 2),
            "&&" => (Tok::AndAnd, 2),
            "||" => (Tok::OrOr, 2),
            "->" => (Tok::Implies, 2),
            "//" | "++" => return unsupported(pos, format!("operator `{two}`")),
            _ => match c {
                '+' => (Tok::Plus, 1),
                '-' => (Tok::Minus, 1),
                '*' => (Tok::Star, 1),
                '/' => (Tok::Slash, 1),
                '<' => (Tok::Lt, 1),
                '>' => (Tok::Gt, 1),
                '!' => (Tok::Bang, 1),
                '(' => (Tok::LParen, 1),
                ')' => (Tok::RParen, 1),
                '=' => (Tok::Assign, 1),
                ';' => (Tok::Semi, 1),
                '"' | '\'' => return unsupported(pos, "string literal"),
                '{' | '}' | '[' | ']' | '.' | ':' | '@' | '?' | ',' | '$' | '~' => {
                    return unsupported(pos, format!("`{c}`"))
                }
                _ => return unsupported(pos, format!("character {c:?}")),
            },
        };
        for _ in 0..len {
            advance(&mut i, &mut line, &mut col, &chars);
        }
        out.push((tok, pos));
    }

    out.push((Tok::Eof, Pos { line, col }));
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn toks(s: &str) -> Vec<Tok> {
        lex(s).unwrap().into_iter().map(|(t, _)| t).collect()
    }

    #[test]
    fn lexes_the_subset() {
        assert_eq!(
            toks("let x-y' = 1 + 2; in if x-y' <= 3 then !true else -4 # c\n/* c */"),
            vec![
                Tok::Let,
                Tok::Ident("x-y'".into()),
                Tok::Assign,
                Tok::Int(1),
                Tok::Plus,
                Tok::Int(2),
                Tok::Semi,
                Tok::In,
                Tok::If,
                Tok::Ident("x-y'".into()),
                Tok::Le,
                Tok::Int(3),
                Tok::Then,
                Tok::Bang,
                Tok::Ident("true".into()),
                Tok::Else,
                Tok::Minus,
                Tok::Int(4),
                Tok::Eof,
            ]
        );
    }

    #[test]
    fn refuses_what_it_cannot_lex() {
        assert!(lex("1.5").is_err());
        assert!(lex("\"s\"").is_err());
        assert!(lex("{ a = 1; }").is_err());
        assert!(lex("[ 1 ]").is_err());
        assert!(lex("a // b").is_err());
        assert!(lex("99999999999999999999").is_err());
    }
}
