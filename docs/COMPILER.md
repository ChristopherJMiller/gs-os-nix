# The on-target compiler (L5)

L5 is the 65816 turning Nix text into the bytecode it runs, with no host in
the loop: the last thing between the VM and M3 (an expression typed at the
IIgs evaluates with zero host involvement). Until now `tools/nbc` on the host
has compiled every program, as a test fixture only (D7). This page is the
design, written before the code.

## What it is

**A port of `nbc`, in C (D33, Chris's choice).** The same four stages,
compiled by ORCA/C into code banks of their own, as the VM is:

| Stage | Host source | Target | Gate: diffed against the host, stage by stage |
|---|---|---|---|
| 1. lexer | rnix 0.14.0 `tokenizer.rs` (566 lines) | `lex.c` | every token: kind, start, length |
| 2a. parser | rnix `parser.rs` (834) | `parse.c` | rnix's syntax tree: every node and token, in preorder |
| 2b. lowering | rnix's typed accessors (`ast/*.rs`) and `front.rs` (1,116) | `lower.c` | the AST, as an S-expression both sides print |
| 3. compiler and image | `compiler.rs` (1,220) and `nbc.rs` (294) | `compile.c` | the `.nbc` image, byte for byte |
| 4. M3 | -- | `repl.c` | a typed line evaluates and the VM prints it |

**The oracle is `nbc`**, rnix and all: D7's exit. An image the target writes
must be the one `nbc` writes for the same source, byte for byte, so every
corpus test and fixture is a test of the compiler without a single new
expected answer -- and every result the scoreboard already holds stays the
judge of what the bytecode means. Where rnix differs from Nix's own parser,
the port follows rnix; such a difference is a bug in the oracle, reported
there, not fixed here first.

**Stage 2 builds rnix's tree, then lowers it** (revised 2026-10-28, before
its code: the first plan merged the parser into the lowering, building
`ast::Expr` nodes directly). `front.rs` does not read a parse; it reads
rnix's syntax tree through rnix's typed accessors -- "the first child node
that casts to an expression", "the first token of this kind" -- and those
accessors are what decide what an error-free but odd tree means: a `(` whose
closing token is whatever followed the expression, a path with whitespace
inside its interpolation. Merging would mean re-deriving each accessor's
answer for every such tree by hand; building the same tree and porting the
accessors over it is mechanical, and each half has a diff of its own.

## Where it runs

In the VM's binary, before `load()`: the source text goes in, an image comes
out in the image bank (bank 3), and the VM runs it as it runs `nbc`'s today.
A2min gives each stage a mode: `--lex` and `--ast` dump a stage's output for
the differential scripts, `--compile` writes the image back to a host file,
and `--run-nix` compiles a `.nix` on the target and runs it, the result
printed as `--run-nbc` prints one. The source arrives in its own bank, as
the image does now.

**Memory.** The source text (up to 64 KB) and the parser's arena and the
compiler's tables are banks the VM does not use yet, at the top of a2min's
1 MB, so code keeps filling banks upward from 4 as omf2bin places it (and
skips them, as it skips 5 and 7; `bank-gate` checks no symbol lands in one).
The AST is an arena, reset per compilation: nothing is collected, because
nothing in it outlives the compilation. The parser recurses as the grammar
nests; the C stack's 19 KB (D31) bounds that, and it is `E_NOMEM` past it,
as nesting is in the VM.

**Imports** are resolved at compile time, as `nbc` resolves them: the
imported file is read (through GS/OS, GSOS.md) and compiled into the same
image, one chunk per file however many sites import it. `derivation.nix` is
embedded as `nbc` embeds it, as its source text.

**Errors.** Input `nbc` refuses, the target refuses: a parse error is
`E_PARSE`, by name; anything `nbc` reports as unsupported is `E_UNSUP`.
`nbc` reads its input as UTF-8 and refuses a file that is not; so does the
target. Positions are line and column as `nbc` counts them (rnix's byte
offsets, lines from `\n`, columns in characters).

## Stage 1: the lexer

A token-for-token port of rnix 0.14.0's tokenizer: its context stack
(string bodies, interpolations with their brace counts, paths), its path,
search-path and URI heuristics, numbers, operators, keywords, comments and
whitespace as tokens, and its error tokens. rnix steps by Unicode
character, not byte, in three places the port must too: whitespace is
Unicode `White_Space` (`char::is_whitespace`: U+0009-000D, 0020, 0085,
00A0, 1680, 2000-200A, 2028, 2029, 202F, 205F, 3000), an error token is one
whole character, and `chars().nth(1)` is the second character.

The gate (`scripts/front-diff.py`, `checks.front`, `just front-diff`): `nbc
--tokens` prints rnix's token stream for a file; `a2min --lex` prints the
target's; they must be identical for every `.nix` in the corpus and the
fixtures, for `derivation.nix`, and for a fuzzer's mutations of them -- the
inputs nobody writes on purpose, where a port goes wrong. Its self-test
plants four real ways a port goes wrong (NBSP not whitespace, a `//` in a
path, an unknown character taken as one byte, `''$` ending an indented
string) and each must be caught.

**Done** (2026-10-28): `lex.c`, 6 KB of code in bank 9. All 721 `.nix`
files in the repository give rnix's tokens exactly, and 100,000 mutants
(five seeds) do too; every plant is caught. Measured, ~534 cycles a byte (143
instructions: a few calls a byte, `at()` and `next()`, each with ORCA/C's
32-bit pointer arithmetic), after a start-up of 7,001 cycles:
`derivation.nix`'s 1,954 bytes in 1.27M cycles, under half a second at 2.8
MHz. Bank 0 +42: the lexer's four statics and `vm_entry`'s mode test (+40
cycles a run). Not yet the parser's: the token stream is pulled one token at
a time, so stage 2 needs no buffer of them.

## Stage 2a: the parser

A function-for-function port of rnix 0.14.0's `parser.rs` over rowan's
builder (a stack of finished children, a stack of open parents, checkpoints
that open a parent at a child count): the same nodes, tokens and trivia in
the same places -- rnix's token buffer and trivia buffer included, and its
`{` lookahead, which reads straight from the lexer after whatever the buffer
holds. **One deliberate difference:** rnix recovers from an error and
parses on, and nbc refuses anything with an error, so the tree after one
never matters; the port stops at the first, `E_PARSE`, having taken the same
steps up to it -- so the two agree on whether there is an error at all, and
on the whole tree when there is not. Error nodes are never built.

The gate: `nbc --cst` prints rnix's tree in preorder (depth, kind, start,
length a line) or refuses; `a2min --cst` the VM's. The fuzzer adds, to the
mutants, a generator of Nix that reaches the grammar's corners -- every
operator, pipes, legacy `let`, the contextual `or`, `__curPos`, interpolated
paths, the lookahead's six patterns, patterns bound twice -- with trivia at
token boundaries; 37% of what it writes parses, against 32% of mutants.
Five planted parser bugs must be caught (`-` right-associative, `{ a ? 1 }`
taken for a set, a checkpoint that leaves its trivia outside, `a b or c` not
`((a (b or)) c)`, a pattern bound twice accepted).

**Done** (2026-10-28): `parse.c`, ~7 KB of code in bank 9. All 721 files
give rnix's tree on the first run, and 100,000 mutants and generated
programs over five seeds; every plant caught (the double bind only once the
generator wrote such patterns: nothing in the corpus does). Lexing and
parsing together, ~1,430-1,640 cycles a byte: `derivation.nix` in 2.8M
cycles, about a second at 2.8 MHz. Bank 0 +16, the parser's eight statics;
its token sets are code, not tables, and the dispatch to each stage is in
"front" too (`front_run`), so that a stage costs `vm_entry` nothing.

**How deep it nests**, measured: 511 `let`s or `else if`s -- rnix's own limit
of 512 levels of `parse_expr`, which the port keeps -- and 1,022 nested
lists, where rnix has none (the dump's walk stack ends it). But only **80**
parentheses or lambda bodies: an expression nested through all fourteen
precedence levels costs ~240 bytes of C stack a level, and the C stack ends
it (`E_NOMEM`) where rnix goes on to 511. Precedence climbing -- one
function for the binary operators instead of fourteen -- would cut that
several-fold; the diff would hold it to rnix's trees.

## Later stages

Each is designed here before its code, when it starts. What is known
already: stage 3's image layout is BYTECODE.md's, and `nbc --dump`
disassembles either side's; stage 4 needs a printer on the target -- today
a2min reads the result from the heap and prints it, which is host
involvement -- and a way to read a line.
