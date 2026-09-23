# Status and how to resume

## Layer progress

| Layer | What | State |
|---|---|---|
| **L0** | Toolchain: flake devShell + justfile + CI gate | **done** (AppleCommander/cadius deferred to L2, only needed for disk images) |
| **L1** | `sha256.v`, `storepath.v`, testbench vs NIST and host `nix` | **done — M1 achieved** |
| **L2** | Card in a machine: register map, verilate, a2bus device, patched mame | **done — M2 achieved** (gated by the ROM-free harness; MAME glue written, needs a romset) |
| **L3** | Memory model: heap port, `keycam.v`, `alu64.v`, collector, CPU nursery | **done** — card side (memory engine, `DATA`/`TAG`, `ALLOC`/`RESERVE`, `FOLLOW`, `HASH_STREAM`, `LOOKUP`, `ALU64`, `MARK`/`COMPACT`) and the CPU-side nursery (`target/heap/nursery.s`: bump alloc, Cheney in RAM, one-burst promotion). D6 measured: `just bench-nursery` |
| **L4** | Bytecode VM on 65816, driven by golden fixtures; scoreboard live | **in progress — slice 1 done, scoreboard live.** `sim/a2min.cpp` models 65816 native mode (`checks.a2min-native`, `checks.mutation-a2min`). [BYTECODE.md](BYTECODE.md) defines `.nbc`; `tools/nbc` (Rust, no deps, D16) compiles the slice-1 subset — ints, bools, null, `let`, `if`, all arithmetic/boolean operators, thunk placement per tvix; `target/vm/vm.s` (native mode) evaluates it with lazy thunks, blackholing and in-place update, `MUL`/`DIV` on the card's `ALU64`. The tvix corpus is vendored (370 eval-okay tests, GPL-3 test data) and `checks.scoreboard` runs it: **246 / 405 pass** (207 tvix + 39 local fixtures), 0 fail, 3 error, 133 unsupported by the lowering, 23 notyetpassing. Slices 2 and 3 landed: strings, lists, attribute sets, 48 primops as synthesised lambdas, `tryEval` catch frames, `with` as marked environments, formals/`rec`/attribute-path/legacy-`let` desugarings, `__functor`, and `TAIL_CALL` `tests/corpus/expected-pass.txt` is the ratchet. `checks.fixture-purity` (D7), `checks.nbc-opcodes`, `checks.nbc-tests` gate it |
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
assembly VM stays until the C one passes everything it does.

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
5. **Port the VM (`target/vm/*.c`)**, slice by slice against the scoreboard; then
   resume slice 4 (promotion) in C. The assembly draft of promotion is kept as
   `target/vm/promote.s` and the card driver as `target/vm/card.s` (neither is
   included by `vm.s`) — they are the reference for the C versions.

**Long-term follow-up:** a black-box FOSS native-mode 65816 C compiler (or 16-bit
mode in LLVM-MOS) to remove the last non-free tool. Its own project; not started.

The scoreboard reads **246 / 405**, and 133 of the rest are `unsupported` —
refused by the lowering, not failed by the VM. Since the generator parses all of
Nix (rnix), the refusals are by feature, and the histogram says what would move
the number: path literal 35, float literal 23, `builtins` used as a value 10,
dynamic attribute names 10, `toJSON`/`fromJSON`/`toXML` 13, then a long tail of
single primops (`catAttrs`, `functionArgs`, `mapAttrs`, `splitVersion`,
`hashString`, …). The 3 `error`s: two tests exhaust the 16 KB nursery (one
building a long string, `eval-okay-tail-call-1` allocating 100 000 call
environments — tail calls no longer grow the CPU stack, but every call still
allocates), and `substring` coercing an int. The number rises by widening the
lowering and the VM together, one slice at a time (BYTECODE.md "Slice plan"):

1. ~~Slice 2a: lambdas, application, `assert`~~ **done** — 17 → 37.
2. ~~Slice 2b: strings, lists, attribute sets, primops~~ **done** — 37 → 134.
   Selection on young sets is a CPU-side hash scan; the card's `LOOKUP` waits for
   promotion (slice 4), and *that* is where the CAM gets measured on real code.
3. ~~Slice 3: `tryEval` catch frames, `with`, formals, `rec`, attribute paths,
   legacy `let`, `__functor`, `TAIL_CALL`~~ **done** — 134 → 246. Catching a
   throw un-blackholes every thunk that was under evaluation (the VM keeps a
   force stack for exactly this), so `tryEval` twice on one failing thunk fails
   twice, as in Nix.
4. **Slice 4: the nursery collector, then promotion to the card** (decided with
   Chris: straight to D6, no in-RAM detour as a destination). Steps: (a) ~~precise
   roots~~ done — every frame keeps its objects on a root stack; (b) ~~the Cheney
   collector at bytecode boundaries~~ done — `target/vm/gc.s`, staging in bank 1,
   survivors block-moved back for now; `just fixtures-gc` collects at every
   boundary and all 39 fixtures survive it, and `checks.scoreboard` runs that too;
   (c) **promotion**: the staging area streamed to the card (`RESERVE` + one `DATA`
   burst), roots rewritten to mature addresses, mature reads through copy
   windows, `LOOKUP` for mature sets, and the write barrier. Then measure the CAM
   on real programs.
5. **Slice 5: builtins that touch the store, `import`, paths, floats.**
6. ~~The parser decision~~ **decided: `rnix`** (D16). The generator now parses all
   of Nix and refuses by feature; `just scoreboard` prints a histogram of the
   refused constructs, so pick the next slice from the top of that list.
7. **`eval-fail` corpus.** Vendor it once the VM's error reporting is worth scoring;
   `E_INFREC` already matches Nix for `let x = x; in x`, and `E_THROW` carries
   the message.
8. **Verb economics on real programs.** `a2min --run-nbc` prints cycles and slot
   accesses per fixture; turn that into `checks.verb-economics` with the corpus
   as the workload instead of micro-benchmarks.

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
  this is not a formality.
- **`checks.a2min-native` + `checks.mutation-a2min`** — the 65816 model the VM runs
  on is tested against data-sheet-derived expectations, and that self-test is proven
  to fail when the interpreter is broken. A wrong CPU model would make every VM
  result suspect, so this sits on the same footing as the RTL regressions.

- **`checks.orcac-smoke`** — the C toolchain end to end (D18): ORCA/C under
  `orcarun` compiles `target/tests/smoke.c`, the ORCA linker links it, `omf2bin`
  flattens it, a2min runs it, and the bytes it leaves are checked against values
  worked out by hand. The VM port is built on this, so a miscompile fails here first.
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
- **`checks.nbc-opcodes`** — BYTECODE.md, `opcodes.rs` and `opcodes.inc` are one
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
