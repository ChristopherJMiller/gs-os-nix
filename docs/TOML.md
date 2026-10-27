# builtins.fromTOML

Nix 2.34 parses with **toml11 4.4.0** (the version the host's Nix was built
with), calling `toml::parse(stream, "fromTOML", toml::spec::v(1, 0, 0))`:
TOML 1.0, with every 1.1 relaxation and every extension off. It then walks the
result (`prim_fromTOML`, `primops/fromTOML.cc`):

- a table becomes a set, an array a list, and a boolean, an integer, a float
  or a string the value it is;
- a date or a time is an error, "Dates and times are not supported", unless
  the `parse-toml-timestamps` experimental feature is on, which it is not;
- a key or a string holding a NUL is `forceNoNullByte`'s error.

Every error is "while parsing TOML", an evaluation error `tryEval` does not
catch: here `E_TOML $13`. The argument is `forceStringNoCtx` (`E_TYPE`).

What is TOML and what it means is toml11's, so `target/vm/c/toml.c` is a
**port** of toml11's `parser.hpp` and `syntax_impl.hpp`, procedure for
procedure, under toml11's MIT licence (its notice is with the code). That
covers the grammar's scanners as PEG functions (a sequence all or nothing,
an `either` its first match, repeats greedy, nothing backtracking into an
alternative), `parse_file`, `parse_table`, `parse_array`,
`parse_inline_table`, `parse_key_value_pair` and `insert_value`.

## The rules, measured

Each was measured on the host; the fixtures `eval-okay-fromtoml-rules` and
`eval-fail-fromtoml-*` hold them.

**Tables.** `insert_value` remembers how each table was made: by a header
(`[a]`), implicitly by a header's key path (`[a.b]` makes `a`), by a dotted
key (`a.b = 1` makes `a`), or inline (`{ }`). That decides what may follow:

- An implicit table may be defined by a header once (`[a.b]` then `[a]`). A
  header's table may not be defined again, nor may a dotted key's
  (`a.b = 1` then `[a]`).
- A dotted key may extend only a table that dotted keys made: `[a.b]` then,
  under `[a]`, `b.y = 2` is an error ("reopening a table using dotted keys").
- An inline table is immutable: `a = { x = 1 }` then `a.y = 2` is an error.
- `[[a]]` appends a table to an array of tables. An array of inline tables
  (`a = [ { } ]`) cannot be appended to. A header or a key path through an
  array of tables goes into its **last** table.
- toml11 lets a header reach into a table that dotted keys made (`[a]`,
  `b.c = 1`, then `[a.b.d]`). TOML 1.0 is unclear here, and it is toml11's
  answer that counts.

**Values.**

- Integers may be decimal (no leading zero), hex, octal or binary. Each must
  fit an `int64`: decimal, hex and octal read through an `istringstream`
  (failbit), binary by toml11's own loop (more than 63 significant bits).
- Floats are the decimal text without its underscores, through `istream >>
  double`, which is `strtod`: correctly rounded (`float.c`'s `f_strtod`),
  and past the largest double an error (the stream's failbit), not
  infinity. `inf`, `nan` and their signs are named: `-nan` has its sign bit
  set.
- Strings come in four kinds: basic (escapes `\b \t \n \f \r \" \\ \uXXXX
  \UXXXXXXXX`; `\e` and `\x` are TOML 1.1's, so errors here, and a surrogate
  is an error), literal, and the multi-line forms of each. A multi-line
  string drops one newline straight after its opening quotes. In a
  multi-line basic string a backslash at the end of a line takes the
  whitespace and newlines after it. One or two quotes beside the closing
  three are part of the string (`"""a""""` is `a"`).
- In TOML 1.0 an inline table is one line, with no trailing comma. An array
  may span lines, hold comments, mix types and end with a comma.
- Each key-value pair needs a line of its own (`a = 1 b = 2` is an error).
- A date or a time is an error. A date-time scans in toml11 as an integer
  followed by `-` or `:`, which `guess_number_type` refuses, so the port
  needs no date grammar at all: the answer is an error either way.
- A BOM at the start is skipped, and CRLF line endings are newlines. toml11
  appends a newline if the document lacks one.
- The keys of a set are sorted by their bytes, as Nix's are.

## The port

The document is copied into **bank 7** (the regex engine's bank, VM.md: a
primop's own until it returns) with toml11's newline appended. The tree goes
into an arena after it. A table or an array is a list of items, and each
key and each string is decoded into the arena. Positions are 16-bit offsets.
Nesting is C recursion, so 32 arrays and inline tables inside one another are
the limit (`E_NOMEM` past it; Nix has none). A header's key path, which can
nest tables deeper, is capped at 48 levels when the value is built.

`prim_fromtoml` (`json.c`) parses, then walks the tree twice. The first walk
sizes the whole value exactly: a set is 4 + 12n bytes, a list 4 + 4n, a
string `round4(n) + 8`, an integer or a float 12, and the names are strings
too. One `nursery_reserve` then covers it all, which no collection can
disturb because the tree is in bank 7. The second walk allocates top-down,
filling each set and list directly, with no value stack. The code is
segment `toml`, which landed in bank 8 (bank 6 is full, and `omf2bin` skips
bank 7).

## How it was checked

- **Natively, against toml11 itself.** A C++ harness runs toml11 4.4.0 exactly
  as `prim_fromTOML` calls it and prints a canonical form: keys sorted,
  floats as their bits, strings in hex. `toml.c` runs natively too, with
  `TM_HOST` and a 64 KB buffer for bank 7. All 709 TOML 1.0 documents of
  the toml-test suite agree (toml11 refuses none of the invalid ones; of
  the valid ones it refuses those with dates and times, as Nix does, and
  those with a NUL), and so do 100,000 random ones: suite documents
  mutated by bytes and lines, and documents built from a dozen key names
  so headers, dotted keys and inline tables collide. 0 differ; of the last
  20,000, toml11 never returned on 28 (below), and the port, which never
  timed out, called each an error.
- **Where Nix never answers.** toml11 4.4.0 loops forever in its error
  recovery when a comment it refuses is followed by another comment line:
  `builtins.fromTOML "# \x7f\n# x\n"` (a DEL in the first comment) does not
  return on the host, and ignores SIGTERM, which Nix only checks between
  evaluation steps. Measured: killed after 20 s, nothing printed. The VM says
  `E_TOML`, the error toml11 would report if its recovery ended. This is the
  one place the port does not reproduce its original, and it cannot be a
  fixture, since the host has no answer to compare.
- **On the VM, against the host.** The fixtures above, in every GC mode, and
  the corpus's `eval-okay-fromTOML`.
- **Mutations.** Each rule broken in a copy, and the fixtures catch it.
