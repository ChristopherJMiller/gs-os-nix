# Status and how to resume

## Layer progress

| Layer | What | State |
|---|---|---|
| **L0** | Toolchain: flake devShell + justfile + CI gate | **done** (AppleCommander/cadius deferred to L2, only needed for disk images) |
| **L1** | `sha256.v`, `storepath.v`, testbench vs NIST and host `nix` | **done — M1 achieved** |
| **L2** | Card in a machine: register map, verilate, a2bus device, patched mame | **done — M2 achieved** (gated by the ROM-free harness; MAME glue written, needs a romset) |
| **L3** | Memory model: heap port, `keycam.v`, `alu64.v`, collector, CPU nursery | **done** — card side (memory engine, `DATA`/`TAG`, `ALLOC`/`RESERVE`, `FOLLOW`, `HASH_STREAM`, `LOOKUP`, `ALU64`, `MARK`/`COMPACT`) and the CPU-side nursery (`target/heap/nursery.s`: bump alloc, Cheney in RAM, one-burst promotion). D6 measured: `just bench-nursery` |
| **L4** | Bytecode VM on 65816, driven by golden fixtures; scoreboard live | **in progress — slices 1-4 done, scoreboard live.** `sim/a2min.cpp` models 65816 native mode (`checks.a2min-native`, `checks.mutation-a2min`). [BYTECODE.md](BYTECODE.md) defines `.nbc`; `tools/nbc` (Rust, no deps, D16) compiles the slice-1 subset — ints, bools, null, `let`, `if`, all arithmetic/boolean operators, thunk placement per tvix; `target/vm/vm.s` (native mode) evaluates it with lazy thunks, blackholing and in-place update, `MUL`/`DIV` on the card's `ALU64`. The tvix corpus is vendored (370 eval-okay tests, GPL-3 test data) and `checks.scoreboard` runs it: **246 / 405 pass** (207 tvix + 39 local fixtures), 0 fail, 3 error, 133 unsupported by the lowering, 23 notyetpassing. Slices 2 and 3 landed: strings, lists, attribute sets, 48 primops as synthesised lambdas, `tryEval` catch frames, `with` as marked environments, formals/`rec`/attribute-path/legacy-`let` desugarings, `__functor`, and `TAIL_CALL` `tests/corpus/expected-pass.txt` is the ratchet. `checks.fixture-purity` (D7), `checks.nbc-opcodes`, `checks.nbc-tests` gate it. **D18 (2026-09-22): the VM is now C** (`target/vm/c/`, [VM.md](VM.md)), compiled by ORCA/C under our own harness ([TOOLCHAIN.md](TOOLCHAIN.md)); it passes **248 / 406** (one more than the assembly it replaced: its 64 KB nursery finishes `eval-okay-ind-string`; the 406th test is slice 4's own `eval-okay-hash-collision`), 0 fail, all 40 fixtures under GC stress. The assembly VM was removed at parity (git has it: commit 05ce616 is the last with both), and `target/vm/promote.s` and `card.s` went with it once slice 4 was ported from them. The C is 2.5× slower and 3.4× larger than the assembly — measured, and the next optimisation targets are named in VM.md |
| **L5** | Self-hosted lexer + parser + compiler; host leaves the loop | not started |
| **L6** | `NARHASH`, ATerm, `derivation` primop, string context | not started |
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

M1/M2/M4 are byte-parity assertions against upstream Nix. They are what separate an
implementation from a tribute.

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

The scoreboard reads **271 / 408** (2026-09-24; 0 fail, 6 error, 109
unsupported, 22 notyetpassing), and it takes **12 seconds**: `scoreboard.py`
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
     antiquotation (`./a/${b}`). And **string + path and `"${path}"` are type
     errors until L6**, because in Nix they copy the path into the store and put
     it in the string's context.
   - **Next: string coercion of attribute sets** (`__toString`, `outPath`).
     `eval-okay-coerce-opadd` needs it, and so does M4: interpolating a
     derivation (`"${drv}"`) is an `outPath` coercion.
   - **~10 need real file I/O** — `readFile`, `readDir`, `pathExists`,
     `hashFile`, `builtins.path`. Those belong with the store (L6) and GS/OS,
     not here.

   **Not** column-wise reads of mature sets: that was tried and measured worse
   (item 8). A 4-byte card read costs 2 bus accesses per byte against ~1 for a
   burst, so reading fields back one at a time is dearer than copying the whole
   object — which means `WINDOW_MAX` (item 4d) has to be lifted by a bigger
   bank-5 ring, not by reading less.

   Also still in this slice: floats (23 tests), which need a decision first —
   ORCA/C's `double` is SANE-based, and whether to use it or carry software
   floats changes what a float costs and whether the card ever sees one.
6. ~~The parser decision~~ **decided: `rnix`** (D16). The generator now parses all
   of Nix and refuses by feature; `just scoreboard` prints a histogram of the
   refused constructs, so pick the next slice from the top of that list.
7. **`eval-fail` corpus.** Vendor it once the VM's error reporting is worth scoring;
   `E_INFREC` already matches Nix for `let x = x; in x`, and `E_THROW` carries
   the message.
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
