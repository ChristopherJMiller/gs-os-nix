# Status and how to resume

## Layer progress

| Layer | What | State |
|---|---|---|
| **L0** | Toolchain: flake devShell + justfile + CI gate | **done** (AppleCommander/cadius deferred to L2, only needed for disk images) |
| **L1** | `sha256.v`, `storepath.v`, testbench vs NIST and host `nix` | **done — M1 achieved** |
| **L2** | Card in a machine: register map, verilate, a2bus device, patched mame | **done — M2 achieved** (gated by the ROM-free harness; MAME glue written, needs a romset) |
| **L3** | Memory model: `heap.v`, `cache.v`, `keycam.v`, `alu64.v`, CPU nursery | not started |
| **L4** | Bytecode VM on 65816, driven by golden fixtures; scoreboard live | not started |
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

## Next actions (L2)

1. ~~Input FIFO~~ **done.** 32 entries, READY = "≥16 slots free". Streaming is now
   **1.07 accesses/byte** (~285× over software SHA-256) against 2.00 with per-byte
   polling. The ~100–300× claim holds. Fixed per-call overhead (~72 accesses, mostly
   `PPORT` readout) is now the larger cost — attack that only if it becomes hot.
2. ~~`mame/a2nix.cpp`~~ **written.** Shim over the Verilated model; contains no card
   behaviour. Advances the model by *elapsed emulated time* at 50 MHz rather than a
   fixed count per access, so the card progresses while the CPU works elsewhere.
   Honours `side_effects_disabled()` so the debugger does not advance
   auto-incrementing ports just by looking at them.
3. ~~Patch MAME~~ **done and verified.** `nix/pkgs/mame-a2nix.nix` makes three
   anchored edits (`bus.lua` file list, `cards.cpp` include, `option_add` inside
   `apple2gs_cards()`) and *verifies each one*, so a MAME version bump fails at a
   named anchor rather than silently producing a MAME without the card. The RTL is
   linked as a static library via `ARCHOPTS`/`LDOPTS` (`nix/pkgs/verilated-card.nix`).
4. ~~65816 test program~~ **written and assembling** (407 bytes).
   `target/tests/storepath_demo.s` scans slots for ID `$A2`, runs the CARD-ISA.md
   burst sequence, and prints the 32-character path. `checks.target-asm` keeps its
   hardcoded fingerprint in sync with the golden vectors.
5. **Remaining for M2:** finish the MAME build, confirm `-listslots apple2gs` lists
   `a2nix`, then run the demo binary against it. Needs a user-supplied `apple2gs`
   ROM. Loading via the MAME debugger is the quickest route; a ProDOS disk image
   (AppleCommander/cadius, still unpackaged) is the tidier one.

## Resuming

1. Read [ARCHITECTURE.md](ARCHITECTURE.md) for the shape, then
   [DECISIONS.md](DECISIONS.md) for why — especially **D5** (verbs not data) and **D6**
   (generational boundary is the slot connector). Those two drive everything else.
2. [CARD-ISA.md](CARD-ISA.md) is normative. RTL and `target/` change together.
3. [REFERENCES.md](REFERENCES.md) holds every verified fact with citations. Don't
   re-research; if something is wrong, fix it there and note the impact.
4. Check this table, then `nix flake check`.

## Standing gates

- **`checks.verb-economics`** — every verb's measured MAME cycle cost vs. its 65816
  equivalent. A verb that doesn't pay for its bus crossing gets deleted.
- **`checks.fixture-purity`** — no `.nbc` fixture contains a store path or hash; no
  fixture generator links a crypto library.
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
