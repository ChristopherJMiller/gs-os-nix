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

## Stage 2b: the lowering

`front.rs` and the rnix accessors it calls, over the tree stage 2a builds:
`Expr` nodes as `ast.rs` defines them, in an arena in the front end's banks
that lives for one compilation. What the port has to carry over exactly:

- **The accessors' answers.** `nth(parent, n)` -- the n-th child *node*
  that casts to the wanted kind (`Expr` covers 24 node kinds, `NODE_ERROR`
  and `NODE_ROOT` among them); `token_u` -- the first child token of a
  kind. Every `None` they can give is a refusal `front.rs` names.
- **Strings**: `normalized_parts`, rnix's two passes over an indented
  string (the least indentation of any line, a first line of spaces
  dropped, a last line of spaces dropped) and `unescape`, with every quirk
  of both -- `''\x` and a lone `'` included.
- **Positions**: `pos()` is the line, and the column in characters, of a
  node's first byte. `front.rs` counts from the start of the file each
  time; the port keeps a table of line starts, built once a file.
- **Numbers**: an integer literal past `i64` is refused; a float literal is
  `f_strtod`'s double (correctly rounded, as Rust's parse is) and refused
  where Nix says "invalid float": infinite, or below 2^-1022 but for the
  exact decimal comparison with the midpoint under it that `float_literal`
  makes.
- **Imports**: `import ./f.nix` reads the file then and there (through
  GS/OS on the target: `gsos_import`, every link resolved as `canonicalize`
  resolves them, a directory's `default.nix`) and lowers it into an `Import`
  node keyed by its canonical path; a file that is not there, not a file or
  not UTF-8 is the `import` primop, applied at run time, as nbc compiles it.
  The importer's tree stays while the imported file's is built after it
  (`parse_file_at`, `cst_mark`), and its source after the importer's in the
  source bank. The dump prints the key relative to the compiled file's
  directory, `..` and all, so that a host path and a GS/OS pathname
  compare; `a2min --ast` mounts the host's root at `/h` and names the file
  `/h` and its real path, so every import the host can reach, it can.

**One rnix bug** the port does not copy: its parser can leave whitespace
inside a path node -- `./a ${b}`, where `peek()` skipped the space before
the interpolation -- and its own `extract_path_parts` then panics on the
whitespace token (nbc exits 101). Nix rejects the input. nbc is changed to
refuse such a tree by name before calling `parts()`, and the target
refuses it the same way.

The gate: `nbc --ast` prints the lowered `Expr` as an S-expression -- a node
a parenthesis, names and string parts quoted with every byte outside
printable ASCII, `"` and `\` as `\xNN`, positions as `L:C` -- and
`a2min --ast` the target's, through a printer of its own. They must be
equal, or both refuse. Six planted lowering bugs must be caught (an indented
string keeping a space too many, a merged `rec` set's `inherit` left an
`inherit`, a column in bytes, a float under the midpoint taken, an import
that cannot be read refused, a directory import without its
`default.nix`); the generator writes the floats at Nix's edges for them.

**Done** (2026-10-28): `lower.c`, ~33 KB of code in bank 9, the AST in an
arena in banks 1 and 2 (the nursery and the stage, which nothing uses
before the image runs), the compiler's scratch and import chain in bank 12
(code now stops below it). All 721 files give nbc's AST on the first run
that had imports -- 695 before them, the other 26 all importing -- and
60,000 mutants and generated programs over three seeds, of which nbc lowers
about 30%. Lexing, parsing and lowering `derivation.nix`: 5.1M cycles, the
lowering 2.3M of them, about 1.8 s at 2.8 MHz. Bank 0 +29, the lowering's
statics. Lowering recurses as the AST nests, ~55 bytes of C stack a level:
`else if` chains, `let` chains and lists now end at ~320, where the parser
alone went to 511 and 1,022 (lambdas and parentheses still end at the
parser's 80).

## Stage 3: the compiler

`compiler.rs` and the image writer `nbc.rs` over the AST: the same chunks,
in the same order, with the same constants and the same code, written as
the same image, byte for byte. What has to carry over exactly:

- **Chunk order.** A chunk is claimed when its first use is compiled -- a
  thunk's before its body, an import's before the file, the `builtins`
  set's and each primop's lambda the first time they are named -- and the
  image lists chunks in that order, so the port compiles in the same order,
  case for case, `strict` and `lazy` and `binding` as nbc does.
- **Constants**, per chunk, deduplicated by value -- a string is not a path
  of the same text -- in first-use order.
- **The peephole**: a RETURN right after a CALL makes it a TAIL_CALL.
- **Names**: scopes as nbc's, lexical first, then `true`/`false`/`null`,
  then -- a `with` in scope -- a run-time lookup with the global as its
  fallback, then the globals, `__NAME`, `builtins` itself. A closed thunk
  (an import, the `builtins` set) is compiled with no scopes around it.
- **The tables**: the primops (name, id, arity), the unqualified globals,
  every name in Nix 2.34.9's `builtins`, the value builtins. In C they are
  string literals in code -- a static array would be bank 0's -- generated
  from `opcodes.rs` into `nbc_tables.h` by `scripts/gen-nbc-tables.py`,
  which `checks.nbc-opcodes` runs with `--check` (and `--selftest`, a drift
  planted in each source). An entry is a length byte, the name and a NUL,
  so that a lookup steps over an entry without scanning it.
- **Sorting**: a set's names by bytes, stably, as Rust's `sort_by` sorts.
- **`derivation`**: `derivation.nix`'s text, embedded (in `nbc_tables.h`
  too), lexed, parsed and lowered the first time a program names it -- a
  newline before it, as Nix embeds it, so its positions are a line on --
  then compiled as an import keyed `<nix/derivation-internal.nix>`. The
  lowering is re-entered for it (`lower_text`), its tree after the trees
  built, its AST after the arena's, as nbc lowers it mid-compile.
- **Positions**: the files and (file, line, column) triples
  `unsafeGetAttrPos` reads, a file named by the fixture convention, its path
  from the compiled file's directory (`strip_prefix`, a component at a
  time) or its name alone.

Memory: the chunks' code in bank 7 in 32-byte blocks, a block's number and
30 bytes (GS/OS's path work there is over once the lowering has read every
import); the chunk table, 512 entries of 32 bytes, so that an index is a
shift, the constants, scopes, maps and positions in the AST's arena; the
compiler's registers in the top 256 bytes of bank 12, above the import
chain, so they cost bank 0 nothing. The image is written into bank 3,
where the VM loads one; `a2min --compile <vm> <in.nix> <out.nbc>` writes
it to a host file.

The gate: `front-diff --stage compile` compiles every file with `nbc -o`
and `a2min --compile` and compares the images byte for byte, or both must
refuse. The fuzzer's generated programs are closed for it -- a `let`, a
`with { }`, lambdas, `with builtins;` or formals around them -- since the
compiler refuses a free variable as nbc does, and draw on the names the
compiler resolves specially (`builtins.NAME`, `derivation`, `__findFile`,
a self-reference). Ten planted compiler bugs must be caught: a CALL before
a RETURN left a CALL, a name sorted after its extensions, the `builtins`
set unsorted, a number constant not shared, a float lazy without a thunk, a
binding that names itself copied, positions for a set Nix builds without
them, an imported file's positions named for its importer, `derivation.nix`
without its newline, a `with`'s miss on `builtins` without the fallback.

**Done** (2026-10-28): `compile.c`, ~25 KB of code in bank 10 (segment
`compile`; bank 9 is the front end's). Every file nbc compiles, 720 of the
723, gives nbc's image byte for byte, and the three it refuses the target
refuses too; so do the 20,000 mutants and generated programs `checks.front`
makes, 4,901 of which nbc compiles, and 5,000 more on another seed (1,262). End to end -- lexed, parsed, lowered,
compiled and written -- `derivation.nix` alone is 3.6M cycles (1.3 s at
2.8 MHz), a derivation fixture 17.1M (6.1 s), a `with builtins;` file of
1.9 KB 14.9M, the 35 KB `eval-okay-big-constants` 29.6M. That last but one
was 26.6M as first written: `A2MIN_PROFILE` put 29% of it in scanning the
tables (an entry's length now leads it), 2.5% in `~CUMUL2` indexing a
22-byte chunk entry (now 32, a shift), and the rest in the `builtins` set
looking up each primop it had just walked past (now handed its entry) and
the linear constant search (a key a constant, compared first). Bank 0 +1,
its alignment; a 32-bit product in the image writer had linked `~UMUL4`
(111 bytes) before it was taken out. The compiler recurses as the AST
nests: lists end at 220 levels, where the lowering went to 333; every
other shape still ends at the parser's or the lowering's limit.

**Found on the way**, in stage 2a: the parser told `__curPos` from an
identifier by reading `SRC_BASE`, the compiled file's bytes, where an
imported file's token is in its own text -- `__curPos` in an imported file
was a variable. It reads the lexer's text now (`lex_text`), and
`eval-okay-import-curpos` imports one.

## Later stages

Stage 4 is designed here before its code, when it starts. What is known
already: it needs the compiler and the VM in one run -- today `a2min
--compile` hands the image back to the host, and `--run-nbc` loads one --
a printer on the target -- a2min reads the result from the heap and prints
it, which is host involvement -- and a way to read a line. The image is in
bank 3 when the compiler is done, where the VM loads one, and the compiler's
memory (banks 1, 2 and 7, the AST's arena and the code blocks) is the VM's
to take back.
