# Decision log

Why the project is shaped this way. Each entry records what was rejected and the
reason, so settled questions stop costing thought.

---

## D1 — Not a SmartPort storage card

**Rejected.** Solved category. `oliverschmidt/a2retronet` (MIT) already implements a
SmartPort mass-storage controller with 8 drives, and ProDOS block devices cap at
16-bit block numbers (32 MB) regardless. Rebuilding it produces a worse version of an
existing card while teaching only the bus.

Kept as a reference for real-hardware bus timing at L7+, never as the product.

---

## D2 — The card must not compute derivations

**Rejected.** If the card evaluates Nix and builds packages, the Apple II is a serial
terminal with extra steps. Nix already has a name for that arrangement — a remote
builder — and it requires no card.

**Invariant adopted:** the card may do arithmetic and hold memory. It may never make a
policy decision or perform I/O.

---

## D3 — CppNix/Lix cannot be ported

**Impossible, not merely hard.**

- No C++ compiler targets the 65816. `ca65` is an assembler; cc65's C backend is
  6502-only; ORCA/C and APW C are commercial C89.
- Nix needs Boehm GC, SQLite, libcurl, and Boost coroutines.
- Evaluating nixpkgs needs gigabytes of heap; a IIgs tops out near 8 MB.

So the project **reimplements Nix's core** for the 65816. This is tractable because Nix
is far smaller than nixpkgs: a lazy evaluator, SHA-256, the store-path fold, ATerm, and
NAR — a Scheme-interpreter-sized problem.

---

## D4 — No Reduceron-style graph-reduction engine on the card

**Considered seriously, then rejected.** [Reduceron](https://github.com/tommythorn/Reduceron)
is an FPGA softcore for lazy functional programs with hardware GC that beats bytecode
Haskell on a 2.8 GHz PC while running at 91.5 MHz. Tempting, and wrong here:

1. **It optimizes the wrong thing.** Reduceron exists for reduction *speed*. This
   project's bottlenecks are memory capacity, hashing, and the absence of any Nix
   implementation for the machine. The dominant cost is 65816 *software*.
2. **Its thesis is wide parallel memories on one die.** Behind a 1.024 MHz 8-bit slot it
   starves — a 100 MHz wide-memory machine connected through a soda straw.
3. **Nix evaluation is store-bound, not compute-bound.** Reduceron's benchmarks
   (`nfib`, `queens`) are pure kernels. Nix is a build-planning language whose job is
   touching a store: `import`, `readFile`, `pathExists`, `derivation`, `toFile`. The
   clean "reduce until impure, then upcall" story collapses because upcalls would be
   the common case, each one a bus round trip.
4. **It re-creates D2 by a cleverer route.** If the card owns heap, stack, and
   reduction, the 65816 is a marshalling engine with an OS attached.

**One idea survives:** template instantiation as a *coarse verb* (`INSTANTIATE`) — one
command in, one wide card-side structured write. Keep the insight, drop the
architecture.

**Also borrowed:** memo 28's O(survivors) mark-compact algorithm, and memos 7/25/27 for
memory layout and wide-stack design. Algorithms only — no York Lava, no GHC in the flake.

---

## D5 — The bus carries verbs, not data

**Forced by measurement.** One card access (~10–11 fast-cycle equivalents, due to the
1.024 MHz `$Cxxx` throttle) costs about the same as one fast-RAM pointer dereference
(`LDA [dp],Y` = 6–8 cycles).

Therefore offload **chains, searches, and high-volume streaming** — never a single
dereference. This killed the original "card is memory behind a window" design.

**Gate adopted:** `checks.verb-economics` measures each verb's MAME cycle cost against
its 65816 equivalent. A verb that doesn't pay for its crossing is deleted.

---

## D6 — The generational boundary is the slot connector

Lazy evaluation has extreme infant mortality; the bus penalty punishes every crossing.
These two facts cancel: put the **nursery in fast RAM** and **mature space on the
card**, promoting only on survival. Most allocations never cross the bus at all.

This is Symbolics' *ephemeral* GC (Moon 1984) with the generation boundary placed on a
peripheral bus. Corollary: **VM stacks stay in fast RAM** — they are the hottest,
youngest data — which kills the proposed stack push/pop ports.

---

## D7 — Host compiler is a test fixture, not a component

The host may generate `.nbc` bytecode **only** as golden test fixtures, to break the
chicken-and-egg of testing a VM with no compiler. A known-good fixture makes every VM
failure definitionally a VM failure.

Self-hosting (L5) is on the **critical path**. The fixtures then become the oracle for
their own removal: compile the same `.nix` on host and target, diff the `.nbc`, and
byte-identical output proves the on-target compiler correct.

**Gate adopted:** `checks.fixture-purity` — no fixture may contain a store path or
hash, and no fixture generator may link a crypto library.

---

## D8 — SIMD in exactly one place

SIMD pays only for many same-shape items, **already card-resident**, on a
compute-bound operation.

| Candidate | Verdict |
|---|---|
| Attrset key CAM (32 comparators) | ✅ Attacks the hot loop, cheap |
| nixbase32 (32 lanes, 1 cycle) | ✅ Nearly free; the XOR fold is just wires |
| Wide `MEMCMP` (8–16 B/cycle) | ✅ Cheap; Nix compares strings constantly |
| Multi-buffer SHA-256 | ❌ One core at 50 MHz ≈ 50 MB/s, already ~150× faster than the bus can feed. Store volumes here are megabytes. 3–4× gates for an imperceptible win |
| Parallel mark engines | ⚠️ MIMD not SIMD; latency-bound, gated on memory banking. L7 |
| ECC field arithmetic | ❌ One verify per narinfo; 100 ms is invisible |

---

## D9 — No TLS; Ed25519 instead

TLS 1.3 on the card is *feasible* (X25519 and RSA verify are cheap in gates; the hard
parts are X.509 parsing and a ~150 KB root bundle), but **unnecessary**.

Nix's integrity model never depended on TLS: narinfo files are Ed25519-signed against
`trusted-public-keys`, and store paths plus `flake.lock` entries are content-addressed
by SHA-256. So plain HTTP + one Ed25519 verify core is fully sound. Transport is
Marinetti (pure 65c816 TCP/IP) over Uthernet II.

`git+https` flake inputs would still want real TLS, and remain out of scope.

---

## D10 — ECP5, not ice40

The heap needs real SDRAM/DDR3. ice40's largest on-chip memory (UP5K SPRAM, 128 KB)
cannot hold a Nix heap. ECP5 boards ship with SDRAM and the toolchain
(`yosys`/`nextpnr`/`trellis`/`openfpgaloader`) is fully open and already in nixpkgs.

---

## D11 — Tags come from the bank byte

The 65816 is bad at masking and shifting. Instead of tag bits, partition memory into
regions by type and read the type from the **bank byte** of a 24-bit address — the
classic 6502-Lisp region-tagging trick. Free type dispatch. The card side uses
explicit tag bits, where masking is free.

---

## D13 — M2 is gated by a minimal 65816 harness, not by MAME

**MAME cannot be the gating check**, because `apple2gs` refuses to start without all
four of its ROMs — and one of them, `344s0047.bin`, is only the **character-generator
font** (`region="gfx1"`, confirmed via `mame -listxml`). A headless run that reads its
answer out of memory never draws a glyph, so that ROM is required bureaucratically
rather than functionally. Those ROMs are copyrighted and not redistributable, so a
MAME-only M2 could never run in CI or on a fresh clone.

Note also that archived MAME romsets stop at 0.278, which still carries the superseded
`megaii.chr` (CRC `ef0bfc11`) rather than the later real dump `344s0047.bin`
(CRC `2d541944`), so even a complete 0.278 set does not satisfy 0.289.

**So `sim/a2min.cpp` supplies the smallest thing that can execute the real binary:**
1 MB of RAM, a slot decoder, and a 65816 interpreter. It runs the *same* `.bin`
and the *same* RTL, with no ROM and no copyrighted material.

What it is and is not:

- It **is** a test harness: real machine code, real bus cycles, real RTL, modelling
  the 50 MHz card clock against the 2.8 MHz CPU and the 1.024 MHz `$Cxxx` stretch.
- It is **not** an Apple IIgs and does not pretend to be — no video, no ROM, no soft
  switches, no interrupts. It began as an emulation-mode-only 65C02 subset; native
  mode (16-bit registers, `D`/`DBR`/`PBR`, the long addressing modes, `JSL`/`RTL`,
  block moves) was added before L4 because the VM needs it. Its self-test
  (`checks.a2min-native`, 39 cases) keeps the expected values in the harness, worked
  out from the data sheet, rather than in the assembly — so the test and the
  interpreter cannot share a mistake — and `checks.mutation-a2min` breaks the
  interpreter 18 ways to prove the self-test notices.
- Unknown opcodes **abort**. A silently-skipped instruction would produce a
  wrong-but-plausible digest, which is the failure mode this project guards hardest
  against. Decimal-mode ADC/SBC and the interrupt instructions (`BRK`, `COP`, `RTI`,
  `WAI`) abort for the same reason.

MAME stays as the **higher-fidelity confirmation** (`just m2-mame`) for anyone with a
complete romset: real slot timing, real GS/OS, the genuine FPI clock stretch. The work
on `mame/a2nix.cpp` is not wasted — it is simply no longer load-bearing for the gate.

---

## D12 — Tree-PLRU, not true LRU

True LRU requires maintaining full ordering across ways: real gates for negligible
gain. 4-way set-associative with tree-PLRU (3 bits/set) gets near-LRU quality. For
pointer-chasing workloads **associativity matters more than policy precision**.

The store region needs no policy discussion at all — it is immutable and
content-addressed, so it is a pure read cache with no invalidation or dirty bits.

---

## D14 — Regression tests must be mutation-tested

Every bug found in this card so far shared a shape: it produced a **plausible-looking
wrong answer** rather than a failure. A byte-reversed digest is still 32 hex bytes; a
truncated message still hashes; a rotated `LEN` still streams; an `ALLOC` header whose
tag byte survived still looked almost right. For a hash accelerator that is the worst
available failure mode, because the caller cannot notice.

So each one gets a permanent named test in `sim/regress_main.cpp` with its mechanism
recorded — and, more importantly, **`scripts/mutation-test.sh` reintroduces each bug
and asserts the test catches it.** A regression test that cannot distinguish a working
card from a broken one produces confidence without evidence.

This is not hypothetical. It fired immediately:

- R4's first mutation (*"READY reports only the hasher's state"*) **SURVIVED**. That was
  the original bug, but the input FIFO had since decoupled the CPU from the hasher and
  made it structurally impossible.
- R4's second mutation (*"the drain issues a byte while one is in flight"*) **also
  survived**. That hazard is genuinely present in the RTL but **unreachable at this
  clock ratio**: the bus delivers a byte every ~48 card cycles while the drain consumes
  one every 2, so the FIFO is essentially never non-empty across a block boundary.
- R4's *test* was also weak: it compared `HASH_PUSH` against `HASH_STREAM` and treated
  agreement as success. Both paths share the FIFO drain, so a drain bug corrupts them
  identically and they still agree. **Two wrong answers that match are not evidence.**
  It now checks both paths against absolute vectors.

Two consequences worth keeping:

1. **The 32-entry FIFO is heavily over-provisioned** for a 1.024 MHz slot, so the whole
   overflow/double-issue hazard class is currently unreachable. Those guards stay as
   defence in depth — they would matter on a slower card clock or a faster bus — but
   they cannot be mutation-tested today, and pretending otherwise would recreate the
   decorative-test problem.
2. **A non-matching mutation pattern is a failure, not a skip.** An inert mutation would
   report "caught" forever while testing nothing.

---

## D15 — The card's collector is a Cheney semispace copier

The plan called for "memo 28's O(survivors) mark-compact". Taken literally, sliding
mark-compact is **O(heap)**: to slide live objects down it must walk every dead one.
What memo 28 actually buys is a collector whose cost scales with what survives, and
the canonical way to get that is **copying**: trace from the roots, copy each live
object into an empty space, never look at garbage at all.

So the heap is two halves. `MARK` evacuates the given roots into to-space and runs
the Cheney scan; `COMPACT` flips the halves and resumes allocation after the survivors.
Splitting them lets the 65816 hand over several root sets — its stack, its nursery's
references — before committing the flip.

Costs accepted, and why:

- **Half the heap is reserved.** On SDRAM that is cheap; on the 1 MB harness heap it
  is 512 KB a side, ample for tests.
- **Every object is at least 8 bytes** so a forwarding pointer always fits. Empty
  objects (`NULL`, `BLACKHOLE`) pay 4 bytes each.
- **The caller must pass every live pointer as a root.** Anything it holds that was
  not in a root list is dangling after `COMPACT`. This is the same contract as any
  moving collector, and the nursery layer (D6) is what makes it tractable: the 65816
  does not hold raw mature-space pointers casually.

Rejected: sliding compaction (O(heap), and no better locality than copying here), and
a non-moving mark-sweep (O(heap) sweep, plus fragmentation that a bump allocator cannot
tolerate).

The one place a copying collector can go silently wrong is tracing a non-pointer as a
pointer. `ATTRS` stores 32-bit hashes that look exactly like addresses, so the scanner
skips that column by construction, and there is a mutation to prove the test notices.

---

## D16 — C++ at the Verilator boundary, Rust for host tools that never touch it

**Decision:** anything that instantiates the Verilated card (`sim/*.cpp`,
`mame/a2nix.cpp`) stays C++. Host-side tools that never touch the model — the
test-only fixture generator `tools/nbc`, and whatever follows it — are Rust.
Glue that merely invokes binaries (`scripts/*.py`, `scripts/*.sh`) stays scripts.

**Why:** Verilator emits a C++ class, and every harness is a thin driver over that
class's ports. Wrapping it for Rust would add a binding layer with no consumer
benefit, and the harnesses' value is precisely that they contain nothing but the
port contract. The generator, by contrast, is a compiler front-end whose next
step is a full Nix parser, and the best one available (`rnix`) is a Rust crate;
starting the subset generator in Rust means swapping the parser later without
rewriting the compiler behind it. Asked whether to rewrite `a2min` in Rust while
adding native mode, the answer was no for this reason.

`tools/nbc` has **no dependencies**, on purpose: `checks.fixture-purity` greps its
manifest for hashing and crypto crates (D7), and a trivial lockfile keeps the nix
package (`nix/pkgs/nbc.nix`) trivial too. That constraint is revisited when the full
parser arrives — and *that* choice (rnix versus growing the subset) is deliberately
left open, to be made with Chris when the subset runs out of corpus.

---

## D17 — Environments are heap objects; thunks capture one pointer

**Decision:** the VM's `let` scopes are ordinary `LIST` objects on the heap
(element 0 = parent, elements 1..n = bindings), variables are resolved at compile
time to `(depth, index)` in that chain, and a thunk captures exactly one pointer —
the current environment. tvix, whose design the VM otherwise follows, resolves
locals to stack slots and copies captured values into each closure and thunk.

**Why:** thunk creation is the hottest allocation in lazy evaluation, and the 65816
has three registers and no block-copy that helps here. Copying `k` upvalues costs
`O(k)` per thunk; capturing the environment costs one 32-bit store. It also makes
recursive `let` free: every thunk sees the *slot*, so forward and self references
need no fix-up pass (tvix's `OpFinalise`), and `let x = x; in x` blackholes and
reports infinite recursion exactly as Nix does. Because environments are ordinary
`LIST`s, both collectors trace and promote them with no special case.

**Cost accepted:** a variable read is a pointer walk of `depth` steps instead of a
stack index. `depth` is the lexical nesting of `let`s, which is small in real Nix,
and every step is in fast RAM (D6) — the nursery is exactly where these live.
Inline caches at bytecode sites (ARCHITECTURE.md) remain the plan for the lookups
that actually hurt, which are attribute selections, not `let` variables.

Full detail, including the thunk-placement table, is in [BYTECODE.md](BYTECODE.md).
