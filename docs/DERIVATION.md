# Derivations (L6, normative)

`derivation` is how a Nix program says what to build, and M4 is the IIgs
producing a `.drv` whose store path matches the host's. This document defines
what the VM reproduces, taken from Nix 2.34.8 (`src/libexpr/primops.cc`,
`src/libstore/derivations.cc`), and **measured against the host** wherever the
source left room for doubt. `scripts/` has no model of it; the fixtures are
the host's own answers (D7).

Scope today: **input-addressed and fixed-output derivations, with inputs,
through string context.** Section "Refused" lists the rest, each a loud
`E_UNSUP` rather than a plausible wrong path.

## Two halves, as in Nix

`derivation` is not a primop. It is Nix code, `derivation-internal.nix`,
wrapped around the primop `derivationStrict`, and the generator embeds that
file verbatim (`tools/nbc/src/derivation.nix`) and compiles it like an import: one
chunk, one `CLOSED_THUNK`, evaluated once however many sites say `derivation`
(BYTECODE.md "Imports"). So the attribute set a program gets back (`all`,
`drvAttrs`, `drvPath`, `outPath`, `outputName`, `type`, one attribute per
output) is Nix's by construction, including its laziness: `d.name` never runs
`derivationStrict` at all.

`derivationStrict attrs` is primop `$31`. It returns `{ drvPath; <output> ...; }`,
store paths as strings.

## What `derivationStrict` computes

**The name** is forced first and must be a `STRING` (not coerced). Then
`__structuredAttrs` and `__ignoreNulls` are forced as booleans if present.

**Every attribute**, in name order:

| attribute | effect |
|---|---|
| `__ignoreNulls` | consumed |
| any, when `__ignoreNulls` and the value is `null` | skipped |
| `__contentAddressed`, `__impure` | consumed **even when false** (a true one is refused) |
| `args` | a list; each element coerced (below) into the builder's arguments |
| `outputHashAlgo`, `outputHashMode` | coerced into the environment, **and read at once**: an invalid mode is an error even with no `outputHash` (section "Fixed outputs") |
| everything else | coerced into the environment under its own name |

`builder`, `system` and `outputs` also go into the environment, and are read from
there: `outputs` is split on whitespace into the output names (default `out`;
duplicates, `drvPath` and an empty set are errors). `builder` and `system` must be
non-empty (`E_NOATTR`).

Measured, because the source reads the other way: `__structuredAttrs = false`
**stays** in the environment (as `""`), while `__contentAddressed = false` and
`__impure = false` do not.

**Coercion** is `coerceToString` with `coerceMore`: a string is itself; a set goes
through `__toString` or `outPath`; `true` is `"1"`; `false` and `null` are `""`;
an integer is decimal; a list is its elements coerced and joined by spaces,
**except that no space follows an element that is an empty list** (Nix's own
comment: "not quite correct"). `toString [ [] "a" [] [] "b" [ [] ] "c" ]` is
`"a b  c"`. A path would be copied into the store: refused (L7).

**The outputs.** Every output name `o` is put into the environment, overwriting an
attribute of the same name, and gets the path name `name` for `out`, else
`name-o`. Each path name, and `name.drv`, must be a valid store-path name: at most
211 bytes, of `[A-Za-z0-9+-._?=]`, not `.`, `..` or starting `.-`/`..-`
(`E_TYPE`). A `name` ending in `.drv` is an error.

### The ATerm

```
Derive([("out","<path>","","")...],[("<drv>",["<output>"...])...],["<src>"...],"<system>","<builder>",["<arg>"...],[("<key>","<value>")...])
```

Outputs are sorted by name, input derivations by path with their output names
sorted, input sources and the environment by bytes. Strings are
quoted, with `"` `\` newline, CR and tab escaped as `\"` `\\` `\n` `\r` `\t`,
**except the output names, the paths (outputs, inputs, sources) and `system`**,
which Nix prints between
quotes with no escaping at all. That is measurable: `system = "x86\"64"` gives a
`.drv` that Nix itself then cannot parse back, yet a read-only evaluation still
reports its path, and the fixture holds the VM to that path.

### Two hashes, three store paths

1. The **masked** ATerm: every output path, and every output's environment value,
   is `""`, and each input `.drv` path is replaced by that input's **modulo
   hash** in hex -- the input list is then sorted by those hex strings, not by
   path. Its SHA-256 is `hashDerivationModulo`.
2. Each output's path is `makeStorePath("output:<o>", that hash, <path name>)`.
3. The **final** ATerm has the real paths and the real inputs. Its SHA-256 gives
   the `.drv`'s path, `makeStorePath("text:<ref>:<ref>...", hash, "<name>.drv")`,
   the references being every input `.drv` and source, sorted (just `text` for
   none).
4. This derivation's own modulo hash, for whatever uses it next, is the SHA-256
   of the final ATerm **with its inputs replaced** as in 1 but its outputs not
   masked. For a derivation with no inputs that is the final ATerm's hash.

Measured: a chain `a` (two outputs) -> `b` (both) -> `c` (`b`) gives the host's
three `.drv` paths with exactly these rules: a model of them was checked
against `nix-instantiate` before any C was written.

`makeStorePath(type, h, name)` is `/nix/store/` + nixbase32(fold(SHA-256(
"<type>:sha256:<hex h>:/nix/store:<name>"))) + `-` + name, where fold XORs the
digest down to 20 bytes (`f[i % 20] ^= h[i]`) and nixbase32 emits from the end.

## Fixed outputs

A derivation with an `outputHash` declares its output's content in advance
(every `fetchurl` is one), so its output path is made from that hash, not from
the derivation, and a derivation that depends on it sees only the hash. From
Nix 2.34.9 (`primops.cc derivationStrict`, `hash.cc`, `content-address.cc`,
`store-dir-config.cc makeFixedOutputPath`, `derivations.cc`), and **every rule
below measured on the host**: a throwaway model of them gave the host's output
path, `.drv` path and dependent's paths for sha256 flat and NAR, sha1 flat and
NAR, md5 and sha512 before any C was written.

**The three attributes** are coerced into the environment like any other, and
two of them are read as soon as they are: `outputHashMode` must be `flat`,
`nar` or `recursive` (= `nar`); `text` and `git` are experimental features,
refused. `outputHashAlgo` is `md5`, `sha1`, `sha256` or `sha512`; `blake3` is
experimental, refused; **anything else is dropped in silence** (Nix's
`parseHashAlgoOpt`), so `outputHashAlgo = "foo"` with an SRI hash is valid.
The derivation is fixed-output only if `outputHash` is present; then its
outputs must be exactly `out` (`E_TYPE` otherwise, even for one output called
something else).

**The hash** (`newHashAllowEmpty`). Empty: all zeros, of `outputHashAlgo`'s
size, which must be given. Otherwise a prefix up to the first `:`, or failing
that the first `-` (which makes it SRI), names the algorithm, and must be a
known one even when `outputHashAlgo` is set; the two must agree when both are
given, and one must be. An SRI hash is always Base64. Otherwise the length
decides, in this order: base16 (`2n`, either case), nix32 (`(8n-1)/5+1`, read
from the end; a set bit past the last byte is an error), Base64
(`4*ceil(n/3)`). Base64 stops at the first `=`, skips newlines and ignores the
bits left over, so `...FU=` and `...FV=` are one hash and so is `...FU=!!`; it
must decode to exactly `n` bytes. Any other length, or a digit outside its
alphabet, is an error (`E_TYPE`).

| algorithm | bytes | base16 | nix32 | Base64 |
|---|---|---|---|---|
| md5 | 16 | 32 | 26 | 24 |
| sha1 | 20 | 40 | 32 | 28 |
| sha256 | 32 | 64 | 52 | 44 |
| sha512 | 64 | 128 | 103 | 88 |

**The output path** (`makeFixedOutputPath`), with `m` the method's prefix (`r:`
for NAR, nothing for flat) and `<hex>` the hash in base16:

- sha256 and NAR: `makeStorePath("source", hash, name)` -- the user's hash is
  the fingerprint's, as for a source added to the store;
- otherwise: `makeStorePath("output:out", SHA-256("fixed:out:<m><algo>:<hex>:"), name)`.

**The ATerm** has the real path in the output and in `out` in the environment
-- there is no masked ATerm -- and the output's other two fields are
`"<m><algo>"` and `"<hex>"`: `("out","/nix/store/...-f","r:sha256","e3b0...")`.
The `.drv` path is the final ATerm's, as for any derivation, so it does depend
on the builder and the inputs.

**The modulo hash** is SHA-256 of `fixed:out:<m><algo>:<hex>:<output path>`,
whatever the inputs. That is the point of the rule: a dependent's masked ATerm
holds this in place of the `.drv`, so two fixed-output derivations with one
hash and different builders give their dependents one output path (and two
`.drv` paths). `eval-okay-derivation-fixed-output` checks exactly that.

The VM never computes md5, sha1 or sha512 for this: the hash is given, and
every hash the rule computes is a SHA-256. The parsed hash is at most 64 bytes,
held in `struct drv` on the C stack; its decoders use constant shifts only
(a variable shift links `~SHIFTLEFT` into bank 0).

## How the VM does it

**Phase 1 runs bytecode, phase 2 does not.** Coercion calls `__toString`, forces
thunks, may collect and may promote. So phase 1 only evaluates. It leaves every
string it produced on the value stack as a root: an `(attribute, string)` pair per
environment entry, in name order; the coerced `args` as a `LIST` on the root stack;
the union of every one of those strings' contexts, which names the inputs; then
the output names, sorted. Phase 2 serialises and hashes with no bytecode, so
nothing moves once it has reserved its memory.

**One reserve, one scratch buffer.** Measuring passes write the masked ATerm to
nowhere, to learn its length, once with the inputs replaced by their modulo hashes
and once with their paths; the unmasked ATerms are longer by exactly twice each
output path. One `nursery_reserve` then covers a scratch buffer of the longest of
those and of the fingerprints (512 bytes for an output, the `.drv`'s grows with
its references), the output-path and `.drv` strings and their contexts, the
`drv_table` entry, and the result set. The scratch buffer takes, in turn, the
masked ATerm, each output's fingerprint, the final ATerm, the `.drv`'s
fingerprint and, with inputs, the final ATerm with them replaced again: each is
consumed before the next overwrites it.

**Inputs in order.** The input `.drv`s are listed by path in the final ATerm
and by modulo hash in the others, and the references by path: the same
elements, three orders. They are taken by selection, the smallest key above the
last, which is quadratic and allocates nothing -- right for the handful of
inputs a derivation has, and the first thing to change for thousands.

**The card.** An ATerm is built young, so `HASH_PUSH` hashes it. A fingerprint
is pushed the same way, and the store path is read from `HASH_FINAL`'s `P3`,
which `storepath.v` folds and encodes for free: the verb M1 proved against Nix. A
store path is one hash with the result read from a different slot. Without a card,
or with the verb masked (`VERB_NO_HASH`), `sha256.c` hashes and folds on the CPU.

**Views.** Phase 2 reads every input string through `deref`, and a mature one is
copied into the window ring. Three passes over a large environment would overrun an
instruction's 44 KB of ring (the concatStringsSep lesson), so the ring is allowed to
turn over **between strings**: at that point the primop holds no view, which is the
same condition a bytecode boundary guarantees.

**Bank 0.** Every static is bank 0 whatever its segment, and so is every ORCA/C
library routine the code links (STATUS.md). So there are two statics,
`drv_table` and the string `"type"` that equality looks up; attribute names
are matched against string literals, which ORCA/C keeps with the code in bank 4
(checked in `cvm.bin`); the hex and nixbase32 alphabets are arithmetic or
literals. The set case of equality moved out of bank 0 with its new rule (bank
0: 39,765 -> 39,345 bytes), and `"${}"` and string `+` with context (-> 38,406).

**Ceilings.** A derivation whose final ATerm, and everything else in the reserve,
does not fit the nursery (~56 KB) is `E_NOMEM`. So is one string longer than
`WINDOW_MAX` (20 KB). At most 8 outputs (`E_UNSUP`).

## String context

Nix remembers, in every string, which store paths it was made from, and that
is how a derivation learns its inputs: `"${a}/bin/sh"` makes `a` an input of
whatever derivation that string reaches. Without it a dependent `.drv` is
well-formed and wrong.

**Representation** (HEAP.md): a `STRING`'s context word is `0` or a `LIST` of
`STRING`s, sorted by bytes, no two equal, each in Nix's own element syntax:
`!<output>!<drv>` one output of a derivation, `=<drv>` all of a derivation (its
`drvPath`), `/nix/store/...` a plain path. The collector already treats the
word as a pointer, on both sides of the bus.

**Made** only by `derivationStrict`: `drvPath` gets `=<drv>`, each output `o`
gets `!o!<drv>`.

**Carried** by every operation that builds a string out of strings, as the union
of their contexts: `+` and `"${}"`, `concatStringsSep` (the separator's too),
`toString` of a list, `substring` (even an empty result), `baseNameOf` and
`dirOf` of a string. The union costs nothing in the usual case -- no context, or
one list shared by every part -- and allocates a merged list otherwise.
**Dropped**, as in Nix, by `hashString`; ignored by `==` and `<`. A string with
context appended to a path is an error, as in Nix.

String building left bank 0 with this: `"${}"` and string `+`, which were
run.c's, are one routine in `prims.c` (bank 4), so no site that must carry
context lives in the bank that is full.

**Consumed** by `derivationStrict`: the union over every coerced attribute and
argument. `!o!<drv>` makes `<drv>` an input derivation with output `o`; a path
makes an input source. `=<drv>` needs the closure of that `.drv`, which the VM
does not keep: refused. The modulo hash of each derivation the program made is
kept in a table (`drv_table`, a root: `[drvPath, hex, next]` lists), and an
input it cannot find there -- a context the program did not make -- is refused
too.

**Not yet:** `getContext`, `hasContext`, `appendContext`,
`unsafeDiscardStringContext`; and Nix rejects context in attribute names
(`listToAttrs`, `getAttr`), which the VM accepts.

## Refused (`E_UNSUP`), and why

`E_UNSUP` ($0E) means *valid Nix that the VM does not implement yet*. It is
distinct from a type error so the scoreboard's error list says what is missing,
and a `refuse-*` fixture passes only on that error (`tests/fixtures`,
`scripts/scoreboard.py`): a `drvPath` in a string, the `text` and `git` hash
modes, `blake3`, a path, and `md5` each have one.

- **A `drvPath` in a derivation's attributes** (`=<drv>` context): Nix adds the
  whole closure of that `.drv`, which the VM does not record.
- **A context the program did not make**: an input derivation missing from the
  table.
- `__structuredAttrs = true`, `__contentAddressed = true`, `__impure = true`.
- `outputHashMode = "text"` or `"git"`, `outputHashAlgo = "blake3"` or a
  `blake3` hash: each is Nix behind an experimental feature (the host rejects
  them without it), and each has a path rule of its own.
- A path in an attribute (copied into the store: L7). Also `"${./p}"`, which was a
  type error until now.

## Equality

Nix compares two sets that both have `type = "derivation"` by `outPath` alone,
when both have one. So `d == d // { x = 1; }` is `true`. The VM does the same.
