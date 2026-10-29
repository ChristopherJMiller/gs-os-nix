# The card's heap, grown and reported — Implementation Plan (Long lists, step 0)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The card's heap halves become a size the card reports (`GEOMETRY`), 8 MB a half by default in a2min and MAME, and the VM uses the reported size instead of `CARD_HALF`, so that programs keeping megabytes live on the card answer instead of `E_NOMEM`.

**Architecture:** The half size becomes an input of `rtl/a2nix_card.v` (`heap_half_log2`, held through reset, tied to a constant on a board, set by each simulator), from which both halves' bounds are computed; a new verb `GEOMETRY` ($26) returns it in `LEN`. The VM asks once when it finds the card and keeps it in `card_half`. a2min takes the size from `A2MIN_CARD_HALF` (default 8 MB), which also lets the tests whose point is filling a half keep running at today's 512 KB.

**Tech Stack:** Verilog (Verilator 5), C++ simulators (`sim/`), ORCA/C for the VM (`target/vm/c`), Python gate scripts, Nix flake checks, `just`.

**Spec:** `docs/HEAP.md` "Long lists (design, 2026-10-29)", **Step 0**. Steps 1 and 2 get their own plans once this lands.

## Global Constraints

- RTL and `target/` change together; `docs/CARD-ISA.md` is normative (README, CARD-ISA.md opening).
- Half sizes are powers of two from 512 KB (`heap_half_log2` = 19) to 8 MB (23). Default 8 MB in a2min and MAME — "the full 16 MB the board keeps, two halves of 8 MB" (HEAP.md).
- The VM reads the size from the card; no `CARD_HALF` constant remains in `target/`.
- A mature pointer stays a card address with bit 31 clear (HEAP.md step 0).
- Bank 0: no new library routines; any new static accounted with `just bank-gate --update` and named in the commit. No 32-bit products in C (`~UMUL4`).
- Gates one at a time, long ones in the background; `git add` every new file before `nix flake check -L --max-jobs 2`; the full flake check passes before each task's commit (the vm-change skill).
- Fixture answers are the host's: `nix-instantiate --eval --strict` (D7).
- HW-12 and rev A's memory map belong to the hardware session: its change is agreed with that session and its document updated in the same commit (HEAP.md step 0).
- Commit messages: what changed, what was MEASURED, what was wrong on the way; the session's trailers.

## Review Focus

1. **A harness that never drives `heap_half_log2`** gets a 1-byte half and every allocation `E_OOM`, which looks like a broken card rather than a missing wire. Each harness should state its size and the regress suite should catch a mismatch — Task 1, Step 1's `l3_geometry`, and each harness setting the input before `reset()`.
2. **`A2MIN_CARD_HALF` given a value that is not a power of two, or out of range,** must be refused loudly, never rounded — Task 2, Step 5.
3. **No card at all (`A2MIN_NO_CARD=1`):** the VM must not issue `GEOMETRY` or use `card_half`. The no-card fixtures answer as before — Task 2, Step 7.
4. **The second flip at 8 MB** (COMPACT back into half A) lands where the half's base says, not at 512 KB — Task 1, Step 1, the log2-23 card.
5. **The tests whose point is filling a half** (`eval-okay-mature-collection`, and `mutation-vm.sh`'s "the card's half fills and nothing collects it") silently stop testing anything at 8 MB. They run at 512 KB, and the check requires the collections to have happened — Task 3, Steps 1–2, and Task 2, Step 8.

---

### Task 1: The half size as an input of the card, and `GEOMETRY`

**Files:**
- Modify: `rtl/a2nix_card.v` — the "Heap geometry" localparams (lines ~105–112), the `from_top` wire (~375), the verb list (~70–85), the verb `case` (~1056)
- Modify: `sim/cardbus.h` — `SPACE_SIZE`/`SPACE_A`/`SPACE_B` (~60), the verb enum (~27), `CardBus` (ctor, `reset()`, a `get_len()`)
- Modify: `sim/regress_main.cpp` — a new `l3_geometry()`, called from `main`
- Modify: `sim/card_main.cpp`, `sim/a2min.cpp` (~328), `mame/a2nix.cpp` (~100) — drive the input
- Modify: `scripts/mutation-test.sh` — two RTL mutations
- Modify: `docs/CARD-ISA.md` — the verb row, a "Heap geometry" note

**Interfaces:**
- Produces: the card port `input wire [4:0] heap_half_log2` (sampled continuously; harnesses set it before reset and hold it); `wire [31:0] space_size = 32'd1 << heap_half_log2`; half A from `$10`, half B from `space_size + $10`; verb `V_GEOMETRY = 8'h26`, Out: `LEN` = `space_size`, done in a cycle. `sim/cardbus.h`: `constexpr unsigned HEAP_HALF_LOG2 = 19;` (the tests' geometry; `SPACE_SIZE = 1u << HEAP_HALF_LOG2` and the rest derived from it), `V_GEOMETRY = 0x26`, `CardBus(CardT *card, HeapMem<CardT> *heap, unsigned half_log2 = HEAP_HALF_LOG2)` which drives the input in `reset()`, and `std::uint32_t get_len()`, which reads the 4 `LEN` bytes after `CTRL <- $08`.

- [ ] **Step 1: Write the failing regress case `l3_geometry()`**

For each `log2` in {19, 23}: a fresh `Va2nix_card`, a `HeapMem<Va2nix_card>` of `2u << log2` bytes, and a `CardBus` built with `log2`, reset. Assertions, each through `check(...)`:
  - `GEOMETRY` completes without `ST_ERR`, and `get_len() == 1u << log2` ("the card reports its half", guards "a harness or card whose half is not the one it was built for").
  - `RESERVE` of `(1u << log2) - 64` succeeds; a following `RESERVE` of 128 is `E_OOM` ("RESERVE stops at the reported half's top", guards "a bound left at 512 KB").
  - With one live INT laid in half A, `MARK` it and `COMPACT`: the root lands at `>= (1u << log2) + 0x10` and `< (2u << log2)`. Then `MARK` and `COMPACT` again: it lands back in `[0x10, 1u << log2)` ("both flips land in the reported halves").

- [ ] **Step 2: Run it and see it fail**

Run: `nix develop -c just regress`
Expected: FAIL. `GEOMETRY` reports `E_BADVERB`, and the build may also fail on the not-yet-existing `heap_half_log2`.

- [ ] **Step 3: Implement the input, the bounds and `GEOMETRY` in `rtl/a2nix_card.v`; drive the input everywhere**

`SPACE_SIZE`/`SPACE_B` become wires from `heap_half_log2`, and `from_top` and the half selection use them. `GEOMETRY` sets `len` and `done`. Every harness drives the input before reset: `sim/cardbus.h` from its ctor argument; `sim/card_main.cpp` and `sim/a2min.cpp` to 19 for now (Task 2 changes a2min); `mame/a2nix.cpp` to 19 (Task 3 changes it). The header comment is updated: "1 MB in the harnesses; a real board sizes it to its SDRAM" becomes an input, set by the board.

- [ ] **Step 4: Run the gates**

Run, one at a time: `nix develop -c just regress` (expected: `REGRESS PASS`, with `l3_geometry`'s checks among them); `just l2-card` or the `checks.l2-card` build (expected: `L2 CARD PASS`); `just rtl-lint` (clean); the `checks.rtl-synth` build (expected: completes).

- [ ] **Step 5: Add two RTL mutations to `scripts/mutation-test.sh`**

"GEOMETRY reports a fixed 512 KB" (`len <= space_size` → `len <= 32'h0008_0000`) and "half B starts at a fixed 512 KB" (half B's base → `32'h0008_0010`). Dry-check each pattern hits: `sed "$expr" rtl/a2nix_card.v | cmp -s - rtl/a2nix_card.v && echo MISS`.
Run: `nix develop -c just mutation`. Expected: `MUTATION PASS`, 32 caught.

- [ ] **Step 6: Document `GEOMETRY` in `docs/CARD-ISA.md`**

Add the row `| $26 | GEOMETRY | — | LEN = bytes a heap half (a power of two) |` to "Heap and memory", and a paragraph stating that the half size is the card's to report (an input, set by the board), that half A starts at `$10` and half B at the size plus `$10`, and that software never assumes a size.

- [ ] **Step 7: Full flake check, then commit**

Run: `git add -A rtl sim mame scripts docs/CARD-ISA.md && nix flake check -L --max-jobs 2` (in the background). Expected: `all checks passed!`. The benchmarks are unchanged, since no software asks the card yet.
Commit: `card: the heap's half size an input, reported by GEOMETRY ($26)`.

---

### Task 2: The VM uses the reported size; a2min at 8 MB a half

**Files:**
- Modify: `target/vm/c/card.c` — `card_find`, a new `card_geometry`
- Modify: `target/vm/c/vm.h` — replace `#define CARD_HALF` (~264) and its comment
- Modify: `target/vm/c/gc.c:320,367` — the two `card_end` computations
- Modify: `sim/heapmem.h` — `resize`
- Modify: `sim/a2min.cpp` — `A2MIN_CARD_HALF`
- Create: `tests/fixtures/eval-okay-card-heap-big.nix`, `.exp`
- Modify: `scripts/mutation-vm.sh` — the geometry for each fixture, one VM mutation
- Modify: `target/vm/bank0.txt` (`--update`), `tests/bench/cycles.txt` (`bench-update`)

**Interfaces:**
- Consumes: `V_GEOMETRY` = $26, Out `LEN` (Task 1).
- Produces: `uint32_t card_geometry(void)` in `card.c` (issues `GEOMETRY`, returns `LEN`); `extern uint32_t card_half;` in `vm.h` (bytes a half; set by `card_find` when a card is found; unused without one); `void HeapMem::resize(std::size_t bytes)`; a2min env `A2MIN_CARD_HALF` (bytes, a power of two in `0x80000`..`0x800000`, default `0x800000`).

- [ ] **Step 1: Write the failing fixture `eval-okay-card-heap-big`**

The program keeps ~3 MB live on the card at once — more than a 1 MB card holds, well within two 8 MB halves:
```nix
# Keeps ~3 MB live on the card at once: 700 forced rows of 100 thunks, every
# row live until the fold ends. A 1 MB card's half cannot hold it (E_NOMEM);
# a card whose halves are bigger, and a VM that uses the size the card reports
# rather than a constant, answers (HEAP.md "Long lists", step 0).
let
  rows = builtins.genList (i: builtins.genList (j: i * j) 100) 700;
in builtins.foldl' (acc: row: acc + builtins.length row + builtins.elemAt row 99) 0 rows
```
`.exp` from the host: `nix-instantiate --eval --strict tests/fixtures/eval-okay-card-heap-big.nix` (measured 2026-10-29: `24290350`).

- [ ] **Step 2: Run it and see it fail**

Run: `nix develop -c just fixtures`
Expected: `eval-okay-card-heap-big` is `error: E_NOMEM` (measured on today's VM: E_NOMEM after one mature collection and 493,532 bytes promoted).

- [ ] **Step 3: Implement `card_geometry`, `card_half`, and `gc.c`'s two bounds**

`card_find` sets `card_half = card_geometry()` once the card answers. `gc.c` keeps its form, `card_end = (at & card_half) + card_half`, valid because the halves are a power of two and half A's base is 0 — say so in a comment. `vm.h`'s comment says where the size comes from.

- [ ] **Step 4: Add `HeapMem::resize`**

- [ ] **Step 5: a2min reads `A2MIN_CARD_HALF`**

Validate it, drive `heap_half_log2`, and size the heap memory to twice the half before `bring_up_card()`. An invalid value prints `A2MIN_CARD_HALF must be a power of two from 0x80000 to 0x800000` and exits 2.

- [ ] **Step 6: Run the fixtures and the scoreboard**

Run: `nix develop -c just fixtures`, then `just scoreboard`. Expected: `eval-okay-card-heap-big` passes, nothing regresses; `just scoreboard-update` claims the new pass.

- [ ] **Step 7: Run the other modes at the default size**

`just fixtures-gc`, the `A2MIN_GC_PERIOD=2` and `=3` passes, `A2MIN_RESERVE_STRESS=1`, `A2MIN_RSET_LIMIT=1`, `A2MIN_VERB_MASK=4`, `just fixtures-mature`, and `A2MIN_NO_CARD=1` through `scripts/scoreboard.py --corpus tests/fixtures`. Expected: the same passes as before plus the new fixture, except where the new fixture cannot run: with no card it is `E_NOMEM`, as `eval-okay-big-operations` is by design, and it is measured and noted if it is past the cycle budget under stress. Also `just scoreboard-target`, `just repl-session`, `just stack-stress`.
Record the total cycles of `just fixtures-mature` at the default size and at `A2MIN_CARD_HALF=0x80000`. A mature collection costs O(survivors), not the half's size, so they should be close. If 8 MB costs markedly more, stop and report it with the numbers before going on: the design keeps 8 MB "unless the measurement of what a mature collection of that much costs says less".

- [ ] **Step 8: `mutation-vm.sh` at both sizes**

`run_one` exports `A2MIN_CARD_HALF=0x80000` for every fixture except `eval-okay-card-heap-big`, which runs at the default, so that every existing fixture tests exactly what it did. Add the mutation "the VM keeps a 512 KB half" (`card_half = card_geometry();` → `card_half = 0x00080000UL;`), which `eval-okay-card-heap-big` must catch. Put the new fixture early in `FIXTURES`.
Run: `nix develop -c just mutation-vm`. Expected: `MUTATION PASS`, one more than before.

- [ ] **Step 9: Bench, bank gate, lint**

Run: `just bench`. Expected: a few cycles and bus accesses more a run, for the one `GEOMETRY` at start; recorded with `just bench-update` and accounted in the commit. Run `just bank-gate`. Expected: bank 0 +4 for `card_half`, nothing else; recorded with `--update`. Run `just c-lint`. Expected: `C LINT OK`.

- [ ] **Step 10: Full flake check, then commit**

Run: `git add -A target tests sim scripts && nix flake check -L --max-jobs 2`. Expected: `all checks passed!`.
Commit: `VM: the card's half as the card reports it; a2min at 8 MB a half`.

---

### Task 3: Keep the half-filling tests honest; MAME at 8 MB; the documents

**Files:**
- Modify: `flake.nix` — the scoreboard check: a step at a 512 KB half; the `just fixtures-mature` equivalent
- Modify: `justfile` — `fixtures-small-card`
- Modify: `tests/fixtures/eval-okay-mature-collection.nix` — its comment (the 512 KB it assumes is now a test setting)
- Modify: `mame/a2nix.cpp` — `HEAP_BYTES` (~93), the input (~100)
- Modify: `docs/HEAP.md` (step 0 marked done, "Collecting mature space"'s sizes), `docs/STATUS.md` (the open item "The card's geometry is a constant" closed; the roadmap), `docs/HARDWARE.md` HW-12 (with the hardware session), `docs/VM.md` if it states the card's size

**Interfaces:**
- Consumes: `A2MIN_CARD_HALF` (Task 2); `GEOMETRY` (Task 1).

- [ ] **Step 1: Write the failing check: the half-filling fixtures at 512 KB must collect**

A step in `checks.scoreboard` (and the recipe `just fixtures-small-card`) that runs `eval-okay-mature-collection` and `eval-okay-tryeval-mature` with `A2MIN_CARD_HALF=0x80000` through `a2min --run-nbc`. It requires each answer to equal its `.exp`, and `eval-okay-mature-collection`'s phases line to report at least 3 mature collections ("N of mature space"; measured before this change: 3). Run it with the default size first and see it fail: at 8 MB a half the plain run collects no mature space.

- [ ] **Step 2: Make it pass at 512 KB, and fix the fixture's comment**

The check runs at `A2MIN_CARD_HALF=0x80000` and passes. The fixture's comment says it is run at a 512 KB half for this, and why.

- [ ] **Step 3: MAME at 8 MB a half**

`HEAP_BYTES = 16u << 20`, and `heap_half_log2 = 23` before reset. Run: `nix build .#mame-a2nix` (a long build; in the background). Expected: it builds. Without a romset nothing runs it, and the check skips as before.

- [ ] **Step 4: HW-12 with the hardware session**

Tell the hardware session (ListAgents / SendMessage, or a note in its plan if none is running) that the core's half is now an input, `heap_half_log2`. MAME and the simulators run 8 MB a half. Rev A ties it, and its SDRAM map already keeps 16 MB. Rewrite HW-12 to match, as agreed.

- [ ] **Step 5: The documents**

HEAP.md: step 0 done with the measurements (`eval-okay-card-heap-big`'s promoted bytes and cycles at 8 MB; bench and bank-0 deltas), and the sizes in "Collecting mature space" and "The VM and the two spaces" stated as the card's. STATUS.md: the open item closed, the roadmap's step 0 struck through. VM.md: any statement of the card's size.

- [ ] **Step 6: Full flake check, then commit**

Run: `git add -A && nix flake check -L --max-jobs 2`. Expected: `all checks passed!`.
Commit: `long lists, step 0: the card's heap at 8 MB a half; the half-filling tests at 512 KB`.
