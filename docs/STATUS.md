# Status and how to resume

## Layer progress

| Layer | What | State |
|---|---|---|
| **L0** | Toolchain: flake devShell + justfile + CI gate | **done** (AppleCommander/cadius deferred to L2, only needed for disk images) |
| **L1** | `sha256.v`, `storepath.v`, testbench vs NIST and host `nix` | **done — M1 achieved** |
| **L2** | Card in a machine: register map, verilate, a2bus device, patched mame | **done — M2 achieved** (gated by the ROM-free harness; MAME glue written, needs a romset) |
| **L3** | Memory model: heap port, `keycam.v`, `alu64.v`, collector, CPU nursery | **done** — card side (memory engine, `DATA`/`TAG`, `ALLOC`/`RESERVE`, `FOLLOW`, `HASH_STREAM`, `LOOKUP`, `ALU64`, `MARK`/`COMPACT`) and the CPU-side nursery (`target/heap/nursery.s`: bump alloc, Cheney in RAM, one-burst promotion). D6 measured: `just bench-nursery` |
| **L4** | Bytecode VM on 65816, driven by golden fixtures; scoreboard live | **in progress — slices 1-4 done, scoreboard live; now 379 / 485 with floats ([FLOAT.md](FLOAT.md)), and the roadmap below runs to a compiler that refuses nothing.** `sim/a2min.cpp` models 65816 native mode (`checks.a2min-native`, `checks.mutation-a2min`). [BYTECODE.md](BYTECODE.md) defines `.nbc`; `tools/nbc` (Rust, no deps, D16) compiles the slice-1 subset — ints, bools, null, `let`, `if`, all arithmetic/boolean operators, thunk placement per tvix; `target/vm/vm.s` (native mode) evaluates it with lazy thunks, blackholing and in-place update, `MUL`/`DIV` on the card's `ALU64`. The tvix corpus is vendored (370 eval-okay tests, GPL-3 test data) and `checks.scoreboard` runs it: **246 / 405 pass** (207 tvix + 39 local fixtures), 0 fail, 3 error, 133 unsupported by the lowering, 23 notyetpassing. Slices 2 and 3 landed: strings, lists, attribute sets, 48 primops as synthesised lambdas, `tryEval` catch frames, `with` as marked environments, formals/`rec`/attribute-path/legacy-`let` desugarings, `__functor`, and `TAIL_CALL` `tests/corpus/expected-pass.txt` is the ratchet. `checks.fixture-purity` (D7), `checks.nbc-opcodes`, `checks.nbc-tests` gate it. **D18 (2026-09-22): the VM is now C** (`target/vm/c/`, [VM.md](VM.md)), compiled by ORCA/C under our own harness ([TOOLCHAIN.md](TOOLCHAIN.md)); it passes **248 / 406** (one more than the assembly it replaced: its 64 KB nursery finishes `eval-okay-ind-string`; the 406th test is slice 4's own `eval-okay-hash-collision`), 0 fail, all 40 fixtures under GC stress. The assembly VM was removed at parity (git has it: commit 05ce616 is the last with both), and `target/vm/promote.s` and `card.s` went with it once slice 4 was ported from them. The C is 2.5× slower and 3.4× larger than the assembly — measured, and the next optimisation targets are named in VM.md |
| **L5** | Self-hosted lexer + parser + compiler; host leaves the loop | not started |
| **L6** | SHA-256 of strings, ATerm, `derivation` primop, string context; `NARHASH` with file I/O (D21) | **in progress** — `builtins.hashString "sha256"` on the card (`HASH_PUSH` for a young string, `HASH_STREAM` for a mature one, `sha256.c` without a card), matching Nix on FIPS padding edges. **`derivation`** ([DERIVATION.md](DERIVATION.md)): input-addressed, byte-identical to the host -- ATerm, `hashDerivationModulo`, output and `.drv` paths from the card's `HASH_FINAL` `P3`; `derivation.nix` embedded verbatim. **String context** carries inputs through `+`, `"${}"`, `concatStringsSep`, `toString`, `substring`, `baseNameOf`, `dirOf`, so a chain of derivations matches the host. **Fixed-output derivations** (`outputHash`, every `fetchurl`): the path from the declared hash (base16, nix32, Base64, SRI; md5, sha1, sha256, sha512; flat and NAR), and the modulo hash a dependent sees, as the host computes them. `drvPath` (`=`) context, the context builtins, `toFile` and `currentSystem` too (Phase B of the roadmap below, complete 2026-10-26). Next for L6: `NARHASH`, which needs file I/O (L7) |
| **L7** | GS/OS driver + FST, Ed25519 + Marinetti fetching, parallel mark | not started |

## Milestones

- **M1** ✅ **achieved** — the RTL emits store paths byte-identical to Linux `nix`.
  12/12 NIST FIPS-180-4 vectors (including the 56- and 120-byte cases that force the
  length field into a second block) and 6/6 store-path vectors, in 4326 cycles.
  Verified by `nix flake check`; e.g. the RTL computes
  `/nix/store/fsl59pk3f4c65048v3w7vgazqwg4d75w-nix-on-an-apple-iigs`.
- **M2** ✅ **achieved** — real 65816 machine code, executing real bus cycles, drove
  the real RTL to a store path identical to upstream nix: 965 instructions, 161 slot
  accesses, `fsl59pk3f4c65048v3w7vgazqwg4d75w`. Gated by `checks.m2-storepath`.
  Runs on `sim/a2min.cpp` rather than MAME — see DECISIONS.md **D13** for why
  (MAME needs a font ROM it cannot legally ship). `just m2-mame` is the optional
  higher-fidelity run for anyone with a complete romset.
- **M3** — A Nix expression typed at the IIgs evaluates with **zero** host involvement.
- **M4** — The IIgs produces a `.drv` whose hash matches the host's exactly.
  **Its shape runs in simulation** (2026-09-25): the C VM on a2min, with the
  Verilated card, computes `/nix/store/76w21n1f03fs5kw8fnffphx7qrqffw6r-hello.drv`
  for a leaf derivation, as the host does -- so its ATerm is the host's to the
  byte (`eval-okay-derivation`: 1.06M cycles and 802 bus accesses for the whole
  program, 9.03M with SHA-256 on the CPU). **With inputs too** (2026-10-25):
  string context makes `a -> b -> c` (two outputs, an input with inputs of its
  own) give the host's three `.drv` paths, and `eval-okay-derivation-context`
  checks 13 more. **Fixed outputs too** (2026-10-25): a `fetchurl`-shaped
  derivation gets the host's output and `.drv` paths, and a derivation built
  on it the host's paths, which depend on its hash and not its builder
  (`eval-okay-derivation-fixed-output`). What M4 still needs is the `.drv` file itself, written to
  disk, which waits for GS/OS file I/O (L7); and the real machine rather than
  a2min.

M1/M2/M4 are byte-parity assertions against upstream Nix. They are what separate an
implementation from a tribute.

## Roadmap (2026-10-26): to a complete bytecode compiler

Agreed with Chris (a goal loop, one item per iteration, each by the vm-change
skill and committed when its gates pass). **Stop condition: `nbc` refuses
nothing in the vendored corpus** -- the "unsupported" histogram is empty, so
every construct and builtin the corpus uses compiles and runs on the VM.
Anything that truly needs GS/OS is refused at run time by name and documented,
not counted against completion. Decisions taken for it: **floats are
soft-float in C** (Chris's choice: bit-identical to the host, no SANE, no
ROM); **`builtins.currentSystem` is `"w65-gsos"`** (chosen on the trajectory
Chris set, a nixpkgs for this platform: GNU `config.sub`'s CPU name for WDC's
65816 line and the kernel a nixpkgs port would add; fixtures from
`nix-instantiate --option system w65-gsos`; DECISIONS.md D23).

Full first-refusal histogram at the start (2026-10-26, corpus incl.
notyetpassing): float literal 25, dynamic attribute name 13, `toJSON` 5,
`toXML` 4, `mapAttrs` 4, `fromJSON` 4, then 3 each `unsafeGetAttrPos`,
`splitVersion`, `pathExists`, `groupBy`, `genericClosure`, `functionArgs`,
`compareVersions`, `catAttrs`; 2 each search-path literal, a missing
`notyetpassing/lib.nix`, `unsafeDiscardStringContext`, `split`, `sort`,
`replaceStrings`, `readFile`, `parseDrvName`, `fromTOML`, `currentSystem`,
`builtins.builtins`; 1 each URI literal, `scopedImport`, a set bound twice,
path antiquotation, `import` as a value, `__curPos`, `toPath`,
`readFileType`, `readDir`, `path`, `match`, `isFloat`, `builtins.import`,
`hashFile`, `hasContext`, `getEnv`, `getContext`, `builtins.fnord`.

**A. Maintainability first** (the C review of 2026-10-25; its tools are in
the session scratchpad and are adopted only after being read):

1. ~~Dead code out of bank 0~~ **done**: `card_name_eq`, `card_memcmp`,
   `gc_maybe`; bank 0 38,406 -> 37,795 bytes.
2. ~~ORCA/C's own lint, `_Noreturn`, `_Static_assert`s~~ **done**: `vm.h`
   turns on `#pragma lint -1` for every VM file (an undefined function, an
   unused variable or a constant a conversion changes stops the build), the
   error exits are `_Noreturn`, and the memory map's invariants are asserts.
   The binary is byte-identical. `checks.orca-lint` plants five violations in
   a copy and requires each to stop the build.
3. ~~A bank-layout gate~~ **done**: `just bank-gate` / `checks.bank-gate`
   hold bank 0 to the exact size in `target/vm/bank0.txt` (37,795) and to an
   allowlist of 16 runtime routines (1,200 bytes), and refuse any symbol in a
   data bank; four plants prove it, one of them a real struct copy. `omf2bin`
   now skips bank 5 when bank 4 fills: tested with two 39 KB segments, the
   second landing at `$060000`, where before it would have been the window
   ring.
4. ~~`checks.c-lint`~~ **done**: `just c-lint` (a few seconds) runs host
   clang `-Wall -Wextra -Werror` on every file through `scripts/hostlint`
   (a shim for `segment` and `asm{}`, line numbers kept), then `gcsafe.py`,
   which computes from the call graph which calls may collect (72 functions)
   or turn the ring over (7 more) and flags an `obj_t *` or a view read after
   one. Its first run found `set_a1` dead too (bank 0 -51, 37,744) and five
   discarded views; gcsafe's two findings are deliberate, mature objects
   never move, and are marked `/* gcsafe: mature */`. Six plants prove it,
   three of them `mutation-vm.sh`'s GC mutations, caught here without an
   ORCA/C build.
5. ~~One list of VM sources; `prims.c` split~~ **done**:
   `target/vm/c/SOURCES` is read by the flake, the justfile and every
   script; `prims.c` (1,996 lines) is now `prims.c` (lists, sets, the table),
   `strings.c` and `derivation.c`, sharing `prims.h`. Code moved byte for
   byte (a line-set diff shows only headers, three macros and `static`).
   Bank 0 +2 bytes: one `~GLOBALS` byte per object file. On the way,
   **`orcalink` passed unresolved references**: the ORCA linker's error
   level is 8 for one and 0 for a clean link, and the wrapper allowed 8. It
   allows 0 now, and `checks.orca-lint` plants an unresolved reference.
6. ~~Integer overflow as Nix 2.34 has it~~ **done**: `+`, `-`, unary `-`,
   `*` and `/` are `E_OVERFLOW` ($0F), an evaluation error `tryEval` does not
   catch, where they used to wrap. Three fixtures from L4 slice 1 asserted the
   wraparound, which the host never did (D7): they are eval-fail fixtures now,
   with six more, and `eval-okay-int-limits` holds 31 answers at the edges
   that must not overflow -- a negative product may reach the minimum, a
   positive one may not. `+`/`-` decide from sign bytes; `*` is exact past
   32 bits (one 64-bit divide, only then). Checks in bank 4: bank 0 +24
   bytes, for the calls. 11 mutations; `checks.nbc-opcodes` now also holds
   the 15 error codes to BYTECODE.md and a2min's positional name table.

**B. L6, what is left before the store** (as listed below): ~~`=<drv>`
context~~ **done** (DERIVATION.md "A .drv in a string": the closure from
`drv_table`'s recorded inputs, matching the host on 10 paths); ~~the context
builtins~~ **done** (DERIVATION.md "Reading and rewriting a context": six
builtins, primops `$32`-`$37`, and no context in a name where Nix forbids
one); ~~`builtins.toFile`~~ **done** (DERIVATION.md "builtins.toFile": the
text store path, and closures that follow its references);
~~`currentSystem`~~ **done** (D23: `"w65-gsos"`, a constant the generator
emits). **Phase B is complete.** **Bank 4 is at 42.9 KB of 64:** the
"prims" segment will need a second segment name before long (omf2bin places
it in bank 6, skipping the window ring).

**C. Coverage, from the top of the histogram:** ~~floats~~ **done**
(2026-10-26, [FLOAT.md](FLOAT.md)): `FLOAT` objects (tag `$0D`, an `INT`'s
shape), float literals, mixed arithmetic and comparison, `isFloat`, `floor`,
`ceil`, `toString`'s `%f` -- soft-float in C, bit for bit the host's.
`float.c` (the arithmetic, alone) and `numbers.c` (the VM's side) are a
segment of their own in bank 4, which now holds 57.8 KB of 64.
`checks.float-vectors` runs float.c built by ORCA/C against 3,360 host
answers (1,000 arithmetic and 120 `%f` vectors per seed, three seeds) and a
planted rounding bug. On the way: **two ORCA/C optimizer miscompiles** of
64-bit `?:` (TOOLCHAIN.md "Known miscompiles"), now refused by
`scripts/hostlint/orcabugs.py`; and Nix's "invalid float" turned out to be
glibc's underflow, tininess after rounding, so `2.2250738585072012e-308` is
invalid though it rounds to the smallest normal (the generator compares
exactly). 18 corpus tests and 10 fixtures; 16 mutations. ~~A primop
printed as a primop~~ **done**: tvix's `eval-okay-value-display`, refused
for its float until then, printed one as `<LAMBDA>`. A primop is a
synthesised lambda, whose code can be the same as a user's, so the
generator marks a primop's chunks (BYTECODE.md "Chunk kinds", in the
header's reserved word) and the printer shows `<PRIMOP>` and
`<PRIMOP-APP>`; the VM never reads them. ~~Dynamic attribute names~~
**done** (BYTECODE.md "Dynamic attribute names"): `{ ${e} = v; }`, `"…${e}…"`
names, and `e.${k}`, `e ? ${k}`, `e.${k} or d`, as Nix 2.34 has them --
`${"a"}` of a plain literal is a static name (Nix's parser), a computed one
is evaluated when the set is, in its scope, never merged, dropped when
`null`, and a clash is `E_DUPATTR` (new, `$10`). Two opcodes, `ATTR_NAME` and
`ATTRS_DYN`, whose work is in bank 4 (bank 0 +14). 10 corpus tests, 15
fixtures, 4 mutations; one of them caught the first version, which took a
computed name that was a selection of a thunk for a type error. ~~The
attribute-set primops~~ **done**: `mapAttrs`, `catAttrs`, `groupBy`,
`zipAttrsWith`, `isPath`, `genericClosure` (primops `$3C`-`$41`), each
forcing what Nix 2.34's primops.cc forces, in its order; 17 corpus tests
(one, `zipAttrsWith`, tvix's own notyetpassing) and 13 fixtures, 9
mutations. `genericClosure` compares a new key with every kept one, O(n^2)
where Nix's `std::map` is O(n log n), and keeps its work list on the value
stack, two slots an element (some 400 elements). **Bank 4 holds 49.6 KB of
`prims`** and the float segment after it, 2.6 KB from full: the next
segment goes to bank 6. **The rest of C, by measure** (the corpus's first
refusals, 2026-10-26, and the host's 118 builtins against nbc's 66): ~~`sort`~~ **done**: Nix 2.34's
own peeksort (`sort.hh`), ported step for step, because an invalid
comparator shows the algorithm (`sort (a: b: true) [ 1 2 3 ]` is `[ 3 2 1
]`); elements and merge buffer on the value stack, `2n` slots. A throwaway
model of the algorithm found inputs that tell each planted bug apart -- the
first fixture's "pseudo-random" lists had every element alike mod 5, so its
"no order" comparator was a constant and a cutoff of 8 for 16 survived. 3
corpus tests, 7 fixtures, 6 mutations; `functionArgs` (the generator must keep
the formals it desugars); `toJSON`/`fromJSON` (a float's shortest
round-trip text), `toXML`, `fromTOML`; ~~`compareVersions`, `splitVersion`,
`parseDrvName`, `replaceStrings`~~ **done** (`names.cc` and `primops.cc` rule
by rule: a run of digits is a version number only while it fits a C++
`int`; a `to` is forced on first use; 10 corpus tests -- one, tvix's
`replaceStrings`, its own notyetpassing -- 11 fixtures, 7 mutations).
**Where tvix's `.exp` is not Nix 2.34's answer** the scoreboard now scores
the host's, from `tests/corpus/host-exp/` (README there; the corpus stays as
vendored, and an override that agrees with tvix is refused): the first is
`replace-strings-propagate-catchable`, where tvix forces every `to`; regex `match`/`split`; `trace`, `warn`,
`break`, `addErrorContext`, `langVersion`, `nixVersion`, `storeDir`,
`placeholder`, `toPath`, `builtins.builtins`, `convertHash`,
`flakeRefToString`/`parseFlakeRef`; positions (`unsafeGetAttrPos`, `__curPos`); search paths, URI
literals, path antiquotation, `import` as a value, `scopedImport`; then the
file builtins, which need GS/OS file calls (a2min implementing the ones
`orcarun` already does is the likely route, and is L7's own first step).

## Roadmap (2026-10-25): from here to M4

The order agreed with Chris (the L6 goal loop): paths as values (done), L6
strings first (D21: `hashString`, ATerm, `derivation`, string context -- done),
then the rest below. Every step: the design in a doc first, the host's answer as
the fixture (D7), a regression that provably fails (D14), the gates one at a time.

**L6, what is left before the store:**

1. ~~**Fixed-output derivations**~~ **done** (2026-10-25,
   [DERIVATION.md](DERIVATION.md) "Fixed outputs"). The output path is
   `makeFixedOutputPath` (`source:` for a sha256 NAR, `fixed:out:` otherwise),
   the ATerm carries `"r:sha256","<hex>"`, and the modulo hash is SHA-256 of
   `fixed:out:<method><algo>:<hex>:<path>`, so a dependent's paths depend on
   the hash, not on how it is fetched. Nix's hash reading is reproduced rule
   by rule from 2.34.9's source and measured on the host first: the prefix,
   SRI, base16 in either case, nix32 with its overflow rule, Base64 that stops
   at `=` and ignores leftover bits, and an unknown `outputHashAlgo` dropped
   in silence. No md5/sha1/sha512 is computed: the hash is given. `text`, `git`
   and `blake3` are experimental features: `E_UNSUP`. One fixture, 14
   `eval-fail` and 3 `refuse` fixtures, and 26 mutations.
2. ~~**`=<drv>` context**~~ **done** (2026-10-26): the `.drv`'s whole
   closure, every `.drv` in it with all its outputs and as a source too,
   computed from the inputs `drv_table` now keeps. Nix itself needs the `.drv`
   in its store for this, so the fixture is generated with
   `--read-write-mode`.
3. ~~**The context builtins**~~ **done** (2026-10-26): `hasContext`,
   `unsafeDiscardStringContext`, `unsafeDiscardOutputDependency`,
   `addDrvOutputDependencies`, `getContext`, `appendContext`; and a context
   is now an error in `getAttr`, `hasAttr`, `listToAttrs`, `removeAttrs` and
   `hashString`'s type, as in Nix. One fixture, 14 eval-fail fixtures, 18
   mutations.
4. ~~**`builtins.toFile`**~~ **done** (2026-10-26): the text store path of
   the contents and their plain-path references, its context itself, and an
   entry in `drv_table` so a closure that reaches it follows those references.
   One fixture (11 host answers, read-write), five eval-fail fixtures, five
   mutations.
5. ~~**`builtins.currentSystem`**~~ **done** (2026-10-26, D23): `"w65-gsos"`,
   what an IIgs is; fixtures generated with `--option system w65-gsos`, so the
   host reports the same. The corpus tests that wait on it use it only as a
   derivation's `system`.

**L7, the store (M4 proper):** GS/OS file I/O, so the `.drv` is written to disk
and `readFile`/`pathExists`/`readDir` work; a path in a derivation copied into
the store, which is where `NARHASH` is first needed and gets priced on real
trees (D21); then the same `.drv` on the real machine (or MAME, D13) rather than
a2min.

**Open, independent of M4:**

- **`LOOKUP`**: a decision for Chris ("Next actions" below). (Floats, the
  other, were decided -- soft-float in C -- and are done: [FLOAT.md](FLOAT.md).)
- **Card-side `MARK`** only when the promoted-bytes counter shows the 512 KB
  ceiling binding on a real program; nothing has yet.
- **Bank 0** (38,203 / 40,960): the value stack (4 KB) and CTAB (2 KB) behind
  far pointers would buy the md5/sha1/sha512 tables. Watch for ORCA/C library
  routines: a struct copy links `~MOVE`, a variable left shift `~SHIFTLEFT`.
- **The `eval-fail` corpus**, and **L5** (self-hosting, D7's exit).

## Next actions (L4)

**Interrupt (2026-09-22, D18): the on-target code moves to C.** Before slice 4's
promotion step continues, the toolchain changed: ORCA/C runs under our own
Golden-Gate-style harness (`docs/TOOLCHAIN.md`), and the VM will be ported to C
with the existing fixtures, corpus and GC-stress mode verifying the port. The
assembly VM stayed until the C one passed everything it did; it has.

1. ~~`nix/pkgs/orcac.nix`~~ **done** — the 2.3.0 B1 release, pinned, extracted
   with cadius; the one unfree package, allowed by name.
2. ~~`sim/orcarun.cpp`~~ **done** — `sim/cpu65816.h` is the core a2min and orcarun
   share; the harness implements the GS/OS, ORCA-shell and Toolbox calls the
   compiler and linker make (aborting on any other), loads OMF including SUPER
   records and jump tables, and runs the real `cc` and `Linker` binaries.
3. ~~`omf2bin`, `checks.orcac-smoke`~~ **done** — `target/tests/smoke.c` compiles,
   links, flattens and runs on a2min with the right bytes (`just orca-smoke`).
4. ~~`orcacc` / `orcalink` / `orcabuild`, `mkOrcaBinary`~~ **done** — in the
   devShell and the flake (`nix/pkgs/orca-toolchain.nix`, `nix/lib/mkOrcaBinary.nix`).
5. ~~**Port the VM (`target/vm/c/`)**~~ **done** — one translation pass, then
   the fixtures found the one bug (the load file's entry is the first function of
   the first object): 40/40 fixtures, 40/40 under GC stress, 248/406 on the
   corpus. Slice 4's promotion followed in C the same day (item 4 below),
   translated from the assembly that had been kept for it. Next: the measured 2.5×
   cost of C ([VM.md](VM.md) "Measured"), then slice 5.

**Long-term follow-up:** a black-box FOSS native-mode 65816 C compiler (or 16-bit
mode in LLVM-MOS) to remove the last non-free tool. Its own project; not started.

**Bank 0: 38,203 of 40,960 bytes** (2026-10-26, after dynamic attribute
names, +14 for two opcode cases; 38,189 after floats: +419, a tag
test in each operation that can see a float -- the work is in bank 4 -- and
`~SHL8`/`~LSHR8`, which ORCA/C links for any 64-bit shift; 37,770 after the
maintainability phase). It was 38,406 (2026-10-25, after string context, and
unchanged by fixed-output derivations, whose 3,263 bytes of code are all in
bank 4; 39,345 after `derivation`, 39,765 before it). String context moved `"${}"` and string
`+` out of run.c into `prims.c`, since every string they build carries a
context, and that took 939 bytes out of bank 0. Code can move into load segments
(`prims.c` and `sha256.c` are `segment "prims"`, bank 4), but **every static,
and every ORCA/C library routine the code links, goes to bank 0**, as the symbol
map shows. String literals stay with their code (checked in `cvm.bin`). So
`derivation` added one static byte and one static string, and the first build
still grew bank 0 by 277 bytes: a struct assignment linked `~MOVE` and a
variable left shift linked `~SHIFTLEFT`, 90 bytes between them, and the
derivation rule in `values_equal` was 167 bytes of bank-0 code. Moving the
whole set case of `values_equal` into `prims.c` (`attrs_equal`) turned that
into a net saving of 420 bytes. That is the lever: code in bank 0 that does not
have to be there. The value stack (4 KB) and CTAB (2 KB) behind far pointers
remain the big one.

The scoreboard reads **467 / 532** (2026-10-26, after the version and name
primops; 0 fail, 6 error, 41 unsupported, 18 notyetpassing). It was 446 /
521 after `sort` (6 error, 50 unsupported). It was 436 / 514 after the
attribute-set primops (7 error, 52 unsupported). It was 406 / 501
after dynamic attribute names (0 fail, 8 error, 67 unsupported, 20
notyetpassing). It was 381 / 486
after the primop printer (0 fail, 8 error, 77 unsupported). It was 379 / 485 after
floats (1 fail, 8 error, 77 unsupported, 20 notyetpassing). Floats brought 18 corpus tests and
10 fixtures. The fail is tvix's `eval-okay-value-display`, refused for its
`42.0` until now and wrong on the printer (`<LAMBDA>` for a primop). The new
error is `eval-okay-types`, which got past its floats to a builtin the VM
lacks, reached through `with builtins;`: there a missing builtin is
`E_NOVAR` at run time, not a refusal, so **the unsupported histogram
undercounts missing builtins** -- `regex-match`, `regex-split` and `sort` are
errors for the same reason. It was 351 / 475 after Phase B (0 fail, 7
error, 97 unsupported, 20 notyetpassing -- `currentSystem` passes both
`delayed-with` tests, one tvix itself does not). It was 342 / 468 after the
context builtins (0
fail, 7 error, 98 unsupported, 21 notyetpassing; three corpus tests pass with
them). It was 324 / 453 after integer overflow (0
fail, 7 error, 101 unsupported, 21 notyetpassing). It was 316 / 445 after
fixed-output derivations -- all 17 new tests then were fixtures, since no
corpus test has an `outputHash` -- and 299 / 428 after string context. New then: its fixtures, and the corpus
test `eval-okay-eq-derivations` -- one tvix itself does not pass -- which compares
five derivations and so waited on the one-derivation guard. It was 295 / 425
after `derivation` and 283 / 413 after paths, string coercion and `hashString`. Two
of the errors are `hashString` with md5, sha1 or sha512, which the card does not
compute (`E_UNSUP`, valid Nix the VM declines): CPU versions are easy, but their
constant tables would have to live in bank 0 (sha512's alone is 640 bytes), so
they wait for bank 0 to be relieved.
Neither is on M4's path. The scoreboard takes about **12 seconds**: `scoreboard.py`
runs tests in parallel and prints the errors and the slowest tests every time.
It was 10 minutes, and 601 s of that was one test,
`eval-okay-cycle-display-cppnix-2.13`, whose cyclic result sent a2min's printer
on a 2⁶⁴-call walk until the 600 s timeout. The printer now prints `«repeated»`
as Nix 2.13+ does, and that test *passes*. The default cycle budget went from
50M to 500M, now ~2.5 s, and `eval-okay-foldlStrict` (86M cycles) passes with it.

Most of the rest are `unsupported`: refused by the lowering, not failed by the
VM. The histogram says what would move the number: float literal 23, path
literal 16, dynamic attribute names 10, `toJSON`/`fromJSON`/`toXML` 13, then a
long tail of single primops (`mapAttrs`, `catAttrs`, `genericClosure`,
`functionArgs`, `groupBy`, `splitVersion`, …). The 6 `error`s: `E_NOVAR` ×4
(`regex-match` twice, `regex-split`, `sort`: missing primops),
`builtins-substring-coerce` (`substring` coercing an int), and
`eval-okay-tail-call-1`, which does not halt even at 2 billion cycles. The
number rises by widening the lowering and the VM together, one slice at a time
(BYTECODE.md "Slice plan"):

1. ~~Slice 2a: lambdas, application, `assert`~~ **done** — 17 → 37.
2. ~~Slice 2b: strings, lists, attribute sets, primops~~ **done** — 37 → 134.
   Selection on young sets is a CPU-side hash scan; the card's `LOOKUP` waits for
   promotion (slice 4), and *that* is where the CAM gets measured on real code.
3. ~~Slice 3: `tryEval` catch frames, `with`, formals, `rec`, attribute paths,
   legacy `let`, `__functor`, `TAIL_CALL`~~ **done** — 134 → 246. Catching a
   throw un-blackholes every thunk that was under evaluation (the VM keeps a
   force stack for exactly this), so `tryEval` twice on one failing thunk fails
   twice, as in Nix.
4. ~~**Slice 4: the nursery collector, then promotion to the card**~~ **done**
   (decided with Chris: straight to D6, no in-RAM detour as a destination).
   (a) ~~precise roots~~ — every frame keeps its objects on a root stack; (b) ~~the
   Cheney collector at bytecode boundaries~~ — `gc.c`, staging in bank 2; `just
   fixtures-gc` collects at every boundary and every fixture survives it, and
   `checks.scoreboard` runs that too; (c) ~~**promotion**~~ (2026-09-22, in C):
   the staging area streamed to the card (`RESERVE` + one `DATA` burst) with every
   pointer field translated, roots rewritten to mature addresses, mature reads
   through a window ring, `LOOKUP` for mature sets with a scan fallback on a hash
   collision (`eval-okay-hash-collision` is two names with one djb2 hash, so it
   fails if the fallback does), and the write barrier for `ENV_SET`, thunk
   updates and deep-force marks ([VM.md](VM.md) "Mature objects",
   [HEAP.md](HEAP.md)). Under GC stress every boundary now promotes, so every
   object of a program is on the card within one instruction of its creation,
   and the fixtures still all pass. Measuring the CAM on real programs needs
   programs that promote by themselves — the corpus's larger tests — and the
   verb-economics report per fixture (item 8).
   (d) ~~**view lifetime**~~ (2026-09-22): the window ring turns over at a
   bytecode boundary and nowhere else, because that is the one moment no view is
   live. The ring it replaced wrapped whenever it filled, which could lay a copy
   on top of a view its caller was still reading — two 30 KB sets under `==`
   filled it exactly — and answer wrongly in silence. Now the only failure is a
   loud `E_NOMEM`, and `WINDOW_MAX` caps a single viewable object at 20 KB
   ([HEAP.md](HEAP.md) “How long a view lives”).
   (e) ~~**the remembered set**~~ (2026-09-23, **D20**): a store into a mature
   object used to promote the value by a whole collection, which made
   `eval-okay-foldlStrict` quadratic. Now the young pointer is written through the
   bus and the field recorded (256 entries, bank 5); the next collection promotes
   it. foldlStrict at `n` = 550 went from 52 collections to 1, and `n` = 1000
   finishes. **What I got wrong:** the set recorded *nothing* on foldlStrict — the
   old barrier fired whenever the value was young, even for an `INT` copied into
   the thunk by value, so most of the quadratic was a barrier with nothing to do.
   **GC stress, as it was, never exercised the set:** promoting at every boundary
   makes every value mature before it is stored. So the stress byte is now a
   period (`A2MIN_GC_PERIOD=k`, collect every *k*-th boundary), and the
   scoreboard check runs the fixtures at periods 1 and 3 and with a one-entry
   set (`A2MIN_RSET_LIMIT=1`, so the full-set path runs). `eval-okay-remembered-set`
   stores young pointers into promoted thunks on purpose, and
   `checks.mutation-vm` breaks the set six ways and requires it to notice.
   **The period mode's first run found an older bug:** empty lists and sets
   were 4 bytes, so a collection wrote their forwarding pointer over the next
   object's header, and `{ } == { }` came out false at periods 2, 5 and 7 (D20).
   Fixed, with two mutations of its own. The rule in ARCHITECTURE.md and CARD-ISA.md now reads
   *"… except through a recorded field"*, and **`MARK`/`FOLLOW` may be issued
   only with the set empty** — one more thing card-side `MARK` must respect.
   **Two findings from building it.** (1) ~~foldlStrict at `n` = 1000 was 2.2×
   slower with the card than without~~ **1.29× now** (2026-09-24), after two new
   instruments named the cost instead of guessing at it:
   - **A symbol map** (`orcalink` passes `+S`; `omf2bin --syms` writes
     `cvm.map`; `A2MIN_PROFILE=1 A2MIN_SYMBOLS=target/build/cvm.map` profiles by
     function, in cycles, with callers). The unnamed hot page was ORCA/C's
     `~LSHR4`: every 32-bit shift in C is a library call at ~300 cycles, and it
     was **28%** of foldlStrict (from `set_a0`, the window hash, `IS_YOUNG`).
     `vm.h BYTE_OF`/`HI16` read the bytes instead; `set_a0` is one asm block.
   - **A bus log** (`A2MIN_BUSLOG=1`: DATA-port reads by card address). **74%**
     of the card traffic was one 4 KB list, copied whole 44 times to read one
     element per iteration: `list_elem` now reads single elements of a long
     mature list that is not in a window (CARD-ISA.md, "when a field read
     wins"). And two hot 12-byte objects shared a direct-mapped window slot and
     evicted each other ~800 times: the window table is 2-way now.

   foldlStrict with the card: **86.3M → 46.0M cycles**, bus accesses 366K →
   124K, card bytes read 238 KB → 45 KB. A GC-stress run of
   `eval-okay-remembered-set`: 133M → 36.6M, after the collector's root walk
   stopped calling `evac`/`translate` for roots that are already mature. What
   remains of the gap is shared VM cost (`deref`, `run`, the call path; the
   "C is 2.5× slower" thread in VM.md), not the card. (2) ~~One instruction
   that allocates more than the free nursery was `E_NOMEM`, depending on where
   the nursery happened to be~~ **fixed** (2026-09-24): `genList 300` allocates
   13 KB in one instruction (44 bytes an element, not the ~8 KB guessed here
   before), and collections ran only at boundaries with a 2 KB margin. Now every
   operation that allocates in proportion to its input calls `nursery_reserve`
   before it pops its operands, and collects there if it must (HEAP.md "When a
   collection happens"). `A2MIN_RESERVE_STRESS=1` makes every reserve collect,
   which is what tests that no call site holds a live pointer in a C local, and
   `eval-okay-big-operations` checks content, not just lengths, because a view
   kept across a collection reads a forwarded object with the right length and
   the wrong elements. The same fixture fails with `E_NOMEM` on the previous
   commit's VM. Two limits it made visible are in HEAP.md: with no card, live
   data that fills the 64 KB nursery made the collector storm (a hang in all
   but name), so a collection that leaves under 8 KB free is now `E_NOMEM`, an
   overhead limit that cannot fire with a card; and a GC-stress run hits the
   card's 512 KB half-space, because mature space is never collected yet. Its profile also found **`toString` at 37%** of that program: ORCA/C's
   64-bit divide (~4,000 cycles) twice per digit. It now divides in 16 or 32
   bits when the value fits (`eval-okay-tostring-widths` checks each boundary).
5. **Slice 5.** The histogram's "path literal 35" is misleading, and measuring
   it changed what this slice is. There are **45** tests whose first refusal is
   a path literal, and they split three ways:

   - ~~**23 of them open with `with import ./lib.nix;`**~~ **import done**
     (2026-09-23): resolved at lowering time, compiled to its own chunk and
     reached through `CLOSED_THUNK`. The refusal histogram moved as predicted —
     path literals 45 → 21 — and the next domino is what `lib.nix` opens with:
     **`with builtins;`**, now the single largest blocker at **29 tests**. That is
     a contained piece of work: the compiler already synthesises a lambda per
     primop, so `builtins` as a value is an attribute set of them.

     What it took: an imported file is evaluated in its **own scope**, but this
     IR carries names symbolically and resolves them in `compiler.rs` against
     whatever encloses them — so inlining an imported file's IR would let it
     capture the importer's `let` and `with` bindings, silently. Hence
     `CLOSED_THUNK` (`env = 0`), and compiling the imported chunk with the
     scope stack emptied, so a free variable there is refused as unbound at
     generation time, as Nix refuses it. Belt and braces on purpose: if the
     compile-time check is ever bypassed, `ENV_GET` in a closed chunk has no
     environment to walk and stops with `E_BADNBC` at the offending
     instruction. Computed imports, interpolated import paths, a shadowed
     `import`, `import` used as a value, and import cycles are all refused by
     name rather than guessed.
   - ~~**~12 are paths as values**~~ **done** (2026-09-24): the generator
     makes each path literal absolute and canonical (Nix's `canonPath`) and emits
     it as a new constant kind, `$05 PATH` (BYTECODE.md). Relative and home paths
     resolve against the fixture convention `/pwd/lang` and `/fake-home`, never
     the disk (D7). The VM has `T_PATH` in `typeOf`, `==`, `<`, `toString`,
     `dirOf` (a path's directory is a path) and `baseNameOf` (a string), and
     `path + string` / `path + path` canonicalise again at run time
     (`data.c path_join`). `obj_size` had no case for `T_PATH`, which has no
     context word, so a promoted path would have been truncated to 4 bytes.
     `eval-okay-paths` is long enough to catch that under GC stress, and two
     mutations prove it. Five corpus tests pass. Still refused: path
     antiquotation (`./a/${b}`). And **string + path and `"${path}"` are
     `E_UNSUP` until L7**, because in Nix they copy the path into the store and
     put it in the string's context.
   - ~~**String coercion of attribute sets**~~ **done** (2026-09-24): Nix's
     `coerceToString` is `prims.c coerce_top`. A set coerces through
     `__toString` (called with the set itself), which takes precedence over
     `outPath`, and the result is coerced again. It applies in `+` (the left
     operand decides what `+` means: a path, an int, otherwise strings, each side
     coerced, left first), in `"${…}"`, in `toString` (with its wider rules),
     and in `substring`, `stringLength` and `concatStringsSep`. M4 depends on
     it: `"${drv}"` is an `outPath` coercion. **Its fixture caught a
     silent wrong answer on the first run:** `concatStringsSep` coerced each
     element to measure it, then copied the original sets' bytes as text.
     `eval-okay-coerce-sets` and two mutations guard it, and the
     `builtins-substring-coerce` and `coerce-opadd` corpus tests pass.
   - **~10 need real file I/O** — `readFile`, `readDir`, `pathExists`,
     `hashFile`, `builtins.path`. Those belong with the store (L6) and GS/OS,
     not here.

   **Not** column-wise reads of mature sets: that was tried and measured worse
   (item 8). A 4-byte card read costs 2 bus accesses per byte against ~1 for a
   burst, so reading fields back one at a time is dearer than copying the whole
   object — which means `WINDOW_MAX` (item 4d) has to be lifted by a bigger
   bank-5 ring, not by reading less.

   ~~Also still in this slice: floats~~ **done** (2026-10-26): decided with
   Chris, soft-float in C, not ORCA/C's SANE `double`; the card never sees
   one ([FLOAT.md](FLOAT.md)).
6. ~~The parser decision~~ **decided: `rnix`** (D16). The generator now parses all
   of Nix and refuses by feature; `just scoreboard` prints a histogram of the
   refused constructs, so pick the next slice from the top of that list.
7. **`eval-fail` corpus.** Vendor it once the VM's error reporting is worth scoring;
   `E_INFREC` already matches Nix for `let x = x; in x`, and `E_THROW` carries
   the message. Our own fixtures already score errors: an `eval-fail-*.nix`
   (Nix rejects it too) or `refuse-*.nix` (valid Nix the VM declines with
   `E_UNSUP`) passes when the VM prints the error line in its `.exp`.
8. ~~**Verb economics on real programs**~~ **done** (2026-09-23) —
   `checks.verb-economics` runs the same fixtures with each verb forced onto its
   CPU path (`vm.h VERB_NO_*`, set by `A2MIN_VERB_MASK`) and diffs the cycles, so
   the gate is answered on real programs rather than micro-benchmarks. It is also
   a correctness check: moving an operation to the CPU must not change an answer.
   Two results, both in [CARD-ISA.md](CARD-ISA.md):
   **Update 2026-09-24: the crossover moved to ~32 attributes** once the VM
   stopped paying ~1,300 cycles to set the card's address register (CARD-ISA.md
   "Measured again (2026-09-24)"); the decision below is still Chris's.
   **`LOOKUP` does not pay below ~70 attributes** — the 2.7x it was justified by
   was against a binary search *through the `DATA` port*, and the VM instead
   copies the set into a window once and scans it in RAM, so the CPU path pays
   nothing per comparison. The verb's own cost is a flat 20 bus accesses at any
   `n`, which is the right hardware shape. **The proposed fix was then built and
   measured, and it made things worse:** confirming the name with the card's new
   `MEMCMP` doubled the verb's cost (−244,812 → −544,391 cycles) because the set
   is already in a window before `find_attr` runs — the caller needs a view to
   check the tag — and because a 4-byte card read costs 2 bus accesses per byte
   against ~1 for a burst. So a CPU search of the cached copy is nearly free and
   the CAM has nothing to beat. **Decision needed (Chris):** D5 says a verb that
   does not pay is deleted; D8 says the attrset CAM is the one place SIMD earns
   its gates. Deleting `LOOKUP` deletes `keycam.v` too.
   **`ALU64` is worth 0.1%** per program though 18x per multiply:
   Nix programs barely multiply. It stays because it is a capability, and the VM
   now also has a software 64-bit divide (`run.c alu_cpu`), which is what lets it
   run with no card at all.

## Resuming

1. Read [ARCHITECTURE.md](ARCHITECTURE.md) for the shape, then
   [DECISIONS.md](DECISIONS.md) for why — especially **D5** (verbs not data) and **D6**
   (generational boundary is the slot connector). Those two drive everything else.
2. [CARD-ISA.md](CARD-ISA.md) is normative. RTL and `target/` change together.
3. [REFERENCES.md](REFERENCES.md) holds every verified fact with citations. Don't
   re-research; if something is wrong, fix it there and note the impact.
4. Check this table, then `nix flake check`.
5. The working method for a VM change -- what to build, which gates to run in
   which order, how to get the host's answer -- is the project skill
   `.claude/skills/vm-change/SKILL.md`.
6. Every external tool and file -- ORCA/C, the disk-image tools, nixpkgs, the
   corpus, and the ROMs you must supply -- is in [SOURCES.md](SOURCES.md), with
   its hash and the terms to find it again.

## Standing gates

- **`checks.regress` + `checks.mutation`** — every silent-corruption bug found gets a
  permanent named test, *and* the mutation runner reintroduces each bug to prove the
  test can fail. See DECISIONS.md **D14**: R4's first two mutations both survived, so
  this is not a formality. **All three mutation runners** (`mutation-test.sh`,
  `mutation-a2min.sh`, `mutation-vm.sh`) work on a copy of the sources in a
  temporary directory and never edit the tree: a SIGKILL skips every trap, and the
  in-place versions twice left a mutation behind. Verified by killing
  `mutation-a2min.sh` with `-9` mid-run and checking `git status`.
- **`checks.a2min-native` + `checks.mutation-a2min`** — the 65816 model the VM runs
  on is tested against data-sheet-derived expectations, and that self-test is proven
  to fail when the interpreter is broken. A wrong CPU model would make every VM
  result suspect, so this sits on the same footing as the RTL regressions.

- **`checks.orcac-smoke`** — the C toolchain end to end (D18): ORCA/C under
  `orcarun` compiles `target/tests/smoke.c`, the ORCA linker links it, `omf2bin`
  flattens it, a2min runs it, and the bytes it leaves are checked against values
  worked out by hand. The VM port is built on this, so a miscompile fails here first.
- **`checks.quiet-clock`** — a2min clocks the card only while that can change
  something (docs/RUNNING.md). That took the simulator from 0.4M simulated
  cycles per second, 7× slower than the IIgs it models, to ~560× faster on
  foldlStrict. The check runs every fixture and the M2 store
  path with and without it (`A2MIN_EXACT_CLOCK=1`), requires byte-identical
  output including every cycle count, and proves it catches a broken copy.
  Measured while choosing what to optimise: an ORCA/C build of the whole VM
  under `orcarun` takes 2.4 s, so the compiler was never the slow part. The
  90-minute mutation runs were a2min, clocking an idle card.
- **`checks.verb-economics`** — every verb's measured MAME cycle cost vs. its 65816
  equivalent. A verb that doesn't pay for its bus crossing gets deleted.
- **`checks.scoreboard`** — the headline metric. Every vendored tvix test is
  compiled by `tools/nbc` and run on the VM under `a2min`; `tests/corpus/
  expected-pass.txt` is a ratchet, so the count can only go up, and the check
  proves the ratchet can fail by planting a regression.
- **`checks.fixture-purity`** — D7 made mechanical: the generator has no hashing,
  crypto or store-path code (its manifest and source are grepped, comments
  stripped), and no compiled `.nbc` carries a store path or digest that is not a
  literal in its `.nix` source. Self-tests by planting each violation first.
- **`checks.nbc-opcodes`** — BYTECODE.md, `opcodes.rs` and `opcodes.h` are one
  table; self-tests by changing one VM opcode in a copy.
- **Every stretch idea sits behind a measurement gate.** Parallel mark engines,
  `NEXTOP` dispatch assist, and stack ports are all unproven. Enthusiasm is not
  evidence.

## Known constraints

- `apple2gs` ROMs are **not redistributable**. User-supplied; MAME-dependent checks
  must skip gracefully without them so CI stays green. GS/OS 6.0.x disk images are
  obtainable.
- The MAME source build is a long one-off, cached afterward. Pin 0.281.
- **On-target code volume is the real cost.** L4–L6 dominate the project; the card is
  a small fraction of the work in any version. D18 moved that code from assembly to
  C (ORCA/C, gratis but not open source — the one unfree package, fetched per
  machine); a FOSS native-mode 65816 C compiler is the long-term follow-up.
