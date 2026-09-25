# Derivations (L6, normative)

`derivation` is how a Nix program says what to build, and M4 is the IIgs
producing a `.drv` whose store path matches the host's. This document defines
what the VM reproduces, taken from Nix 2.34.8 (`src/libexpr/primops.cc`,
`src/libstore/derivations.cc`), and **measured against the host** wherever the
source left room for doubt. `scripts/` has no model of it; the fixtures are
the host's own answers (D7).

Scope today: **input-addressed derivations without string context.** Section
"Refused" lists the rest, each a loud `E_UNSUP` rather than a plausible wrong
path.

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
Derive([("out","<path>","","")...],[],[],"<system>","<builder>",["<arg>"...],[("<key>","<value>")...])
```

Outputs are sorted by name, the environment by key (byte order both). Strings are
quoted, with `"` `\` newline, CR and tab escaped as `\"` `\\` `\n` `\r` `\t`,
**except the output names, the paths and `system`**, which Nix prints between
quotes with no escaping at all. That is measurable: `system = "x86\"64"` gives a
`.drv` that Nix itself then cannot parse back, yet a read-only evaluation still
reports its path, and the fixture holds the VM to that path.

### Two hashes, three store paths

1. The **masked** ATerm: every output path, and every output's environment value,
   is `""`. For a derivation with no input derivations its SHA-256 is
   `hashDerivationModulo`. (With inputs, each input `.drv` path is replaced by
   *its* modulo hash, which needs string context.)
2. Each output's path is `makeStorePath("output:<o>", that hash, <path name>)`.
3. The **final** ATerm has the real paths. Its SHA-256 gives the `.drv`'s path,
   `makeStorePath("text", hash, "<name>.drv")`; with references (inputs, later)
   the type is `text:<ref>:<ref>...`, sorted.

`makeStorePath(type, h, name)` is `/nix/store/` + nixbase32(fold(SHA-256(
"<type>:sha256:<hex h>:/nix/store:<name>"))) + `-` + name, where fold XORs the
digest down to 20 bytes (`f[i % 20] ^= h[i]`) and nixbase32 emits from the end.

## How the VM does it

**Phase 1 runs bytecode, phase 2 does not.** Coercion calls `__toString`, forces
thunks, may collect and may promote. So phase 1 only evaluates. It leaves every
string it produced on the value stack as a root: an `(attribute, string)` pair per
environment entry, in name order; the coerced `args` as a `LIST` on the root stack;
then the output names, sorted. Phase 2 serialises and hashes with no bytecode, so
nothing moves once it has reserved its memory.

**One reserve, one scratch buffer.** A measuring pass writes the masked ATerm to
nowhere, to learn its length. The final ATerm is longer by exactly twice each
output path. One `nursery_reserve` then covers a scratch buffer of the larger of
that and a fingerprint (512 bytes), the output-path and `.drv` strings, and the
result set. The scratch buffer takes the masked ATerm, then each fingerprint, then
the final ATerm, then the `.drv` fingerprint: each one is consumed before the next
overwrites it.

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
library routine the code links (STATUS.md). So there are two statics, the
one-byte guard and the string `"type"` that equality looks up; attribute names
are matched against string literals, which ORCA/C keeps with the code in bank 4
(checked in `cvm.bin`); the hex and nixbase32 alphabets are arithmetic or
literals; and the set case of equality moved out of bank 0 with the new rule,
which left bank 0 420 bytes smaller than before.

**Ceilings.** A derivation whose final ATerm, and everything else in the reserve,
does not fit the nursery (~56 KB) is `E_NOMEM`. So is one string longer than
`WINDOW_MAX` (20 KB). At most 8 outputs (`E_UNSUP`).

## Refused (`E_UNSUP`), and why

`E_UNSUP` ($0E) means *valid Nix that the VM does not implement yet*. It is
distinct from a type error so the scoreboard's error list says what is missing,
and a `refuse-*` fixture passes only on that error (`tests/fixtures`,
`scripts/scoreboard.py`): the second derivation, fixed-output, a path, and
`md5` each have one.

- **A second `derivationStrict` in one program.** Until string context exists,
  one derivation's output path could reach another's attributes without the
  context that makes it an input. The second `.drv` would then be missing its
  `inputDrvs`, and have a well-formed, wrong path: the failure this project
  guards against hardest. One derivation per program cannot consume another. The
  guard goes when context propagation lands (next; D22).
- `__structuredAttrs = true`, `__contentAddressed = true`, `__impure = true`.
- `outputHash`, `outputHashAlgo`, `outputHashMode`: fixed-output derivations.
- A path in an attribute (copied into the store: L7). Also `"${./p}"`, which was a
  type error until now.

## Equality

Nix compares two sets that both have `type = "derivation"` by `outPath` alone,
when both have one. So `d == d // { x = 1; }` is `true`. The VM does the same.
