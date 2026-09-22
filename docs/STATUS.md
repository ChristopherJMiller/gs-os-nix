# Status and how to resume

## Layer progress

| Layer | What | State |
|---|---|---|
| **L0** | Toolchain: flake devShell + justfile + CI gate | **done** (AppleCommander/cadius deferred to L2, only needed for disk images) |
| **L1** | `sha256.v`, `storepath.v`, testbench vs NIST and host `nix` | **done — M1 achieved** |
| **L2** | Card in a machine: register map, verilate, a2bus device, patched mame | **done — M2 achieved** (gated by the ROM-free harness; MAME glue written, needs a romset) |
| **L3** | Memory model: heap port, `keycam.v`, `alu64.v`, collector, CPU nursery | **done** — card side (memory engine, `DATA`/`TAG`, `ALLOC`/`RESERVE`, `FOLLOW`, `HASH_STREAM`, `LOOKUP`, `ALU64`, `MARK`/`COMPACT`) and the CPU-side nursery (`target/heap/nursery.s`: bump alloc, Cheney in RAM, one-burst promotion). D6 measured: `just bench-nursery` |
| **L4** | Bytecode VM on 65816, driven by golden fixtures; scoreboard live | **in progress — slice 1 done, scoreboard live.** `sim/a2min.cpp` models 65816 native mode (`checks.a2min-native`, `checks.mutation-a2min`). [BYTECODE.md](BYTECODE.md) defines `.nbc`; `tools/nbc` (Rust, no deps, D16) compiles the slice-1 subset — ints, bools, null, `let`, `if`, all arithmetic/boolean operators, thunk placement per tvix; `target/vm/vm.s` (native mode) evaluates it with lazy thunks, blackholing and in-place update, `MUL`/`DIV` on the card's `ALU64`. The tvix corpus is vendored (370 eval-okay tests, GPL-3 test data) and `checks.scoreboard` runs it: **37 / 385 pass** (18 tvix + 19 local fixtures), 0 fail, 1 error (deep tail recursion), 324 unsupported by the lowering, 23 notyetpassing. Lambdas, application and `assert` landed as slice 2a. `tests/corpus/expected-pass.txt` is the ratchet. `checks.fixture-purity` (D7), `checks.nbc-opcodes`, `checks.nbc-tests` gate it |
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

The scoreboard reads **37 / 385**, and 324 of the rest are `unsupported` — refused
by the lowering, not failed by the VM. Since the generator parses all of Nix
(rnix), the refusals are by feature, and the histogram says what would move the
number: attribute selection 118 (almost all `builtins.…`), list 61, attribute set
50, `with` 39, string 22, legacy `let` 10, formals 7, `//` 5. (Each test is
counted once, by the first construct refused, so lists hide further attrsets and
strings behind them.) The number rises by widening the lowering and the VM
together, one slice at a time (BYTECODE.md "Slice plan"):

1. ~~Slice 2a: lambdas, application, `assert`~~ **done** — closures capture the
   environment pointer, `CALL` builds a two-slot environment, arguments are lazy.
   17 → 37.
2. **Slice 2b: lists, strings, attribute sets, `builtins`.** `LIST`/`STRING`/
   `ATTRS` construction, `++`, `//`, `.` selection (CPU-side binary search on
   young sets, the card's `LOOKUP` on promoted ones — both paths, measured), `?`,
   interpolation, and the `builtins` attrset with the list/string primops the
   corpus leans on. Needs a deep-force before reporting and a Nix-syntax printer
   in the harness. This is where the CAM earns its 1400 LUT4 on real code.
3. **Slice 3: `with`, `rec`, formals, `TAIL_CALL`, inline caches** at selection
   sites. The scoreboard's one `error` is `eval-okay-tail-call-1`: 100 000 nested
   calls, which needs `TAIL_CALL` *and* a running nursery collector (slice 4).
3. **Slice 4: builtins and `import`**, paths, and the nursery collector running
   *during* evaluation (slice 1 aborts with `E_NOMEM` when 16 KB fills — loud, but
   a limit). Then promotion of survivors to the card, closing the loop with L3.
4. ~~The parser decision~~ **decided: `rnix`** (D16). The generator now parses all
   of Nix and refuses by feature; `just scoreboard` prints a histogram of the
   refused constructs, so pick the next slice from the top of that list.
5. **`eval-fail` corpus.** Vendor it once the VM's error reporting is worth scoring;
   `E_INFREC` already matches Nix for `let x = x; in x`.
6. **Verb economics on real programs.** `a2min --run-nbc` prints cycles and slot
   accesses per fixture; once slice 2 lands, turn that into `checks.verb-economics`
   with the corpus as the workload instead of micro-benchmarks.

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
- **65816 assembly volume is the real cost.** L4–L6 dominate the project; the card is a
  small fraction of the work in any version.
