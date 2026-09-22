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

## D12 — Tree-PLRU, not true LRU

True LRU requires maintaining full ordering across ways: real gates for negligible
gain. 4-way set-associative with tree-PLRU (3 bits/set) gets near-LRU quality. For
pointer-chasing workloads **associativity matters more than policy precision**.

The store region needs no policy discussion at all — it is immutable and
content-addressed, so it is a pure read cache with no invalidation or dirty bits.
