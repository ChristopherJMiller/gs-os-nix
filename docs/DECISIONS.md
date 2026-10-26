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

**Gate adopted:** `checks.verb-economics` measures each verb's cycle cost against
its 65816 equivalent. A verb that doesn't pay for its crossing is deleted.

**The gate now exists and has already changed an answer (2026-09-23).** It runs
the corpus with each verb forced onto the VM's own CPU path and diffs the
cycles. `LOOKUP`, the verb this project was most confident about, turns out not
to pay below ~70 attributes — because its 2.7x was measured against a binary
search *through the `DATA` port*, and the VM the project actually built copies a
mature set into a window once and scans it in RAM, paying nothing per
comparison. So D5 applied to the software as well as the hardware, and in doing
so undercut a verb: the CPU side got good enough that the crossing stopped
paying. The verb is not deleted, because its cost is a flat 20 accesses at any
`n` — the right shape — and the fix is on the software side (confirm the matched
name with `MEMCMP` instead of copying the set). Details in
[CARD-ISA.md](CARD-ISA.md); the same run priced `ALU64` at 0.1% per program
against 18x per multiply.

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

**Parser (decided with Chris, 2026-09-22): `rnix`, from the start.** Slice 1 shipped
with a hand-written subset parser and the scoreboard read 17 / 376 with 336
"unsupported" — refused by the *parser*, so the number measured the generator, not
the VM. `rnix` is the whole grammar (it is what tvix uses), so from here the
generator parses everything and refuses, by *feature*, only what the VM cannot run
yet; the scoreboard histograms those refusals, which is how each next slice is
chosen. The hand-written parser was ~400 lines that rnix would have replaced
anyway, and at L5 an independent, battle-tested parser is the better differential
oracle for the on-target compiler. The cost is a dependency tree (`rnix`, `rowan`)
vendored through the lockfile; `checks.fixture-purity` still greps the manifest for
hashing and crypto crates (D7), and that list is the only thing the manifest may
grow.

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

---

## D18 — The on-target code moves from assembly to C, compiled by ORCA/C under our own harness

**Context (2026-09-22).** The VM (`target/vm/*.s`) is the product, not a test, and
by slice 4 it was ~5.5k lines of 65816 assembly with L5 and L6 still to write.
Nearly every bug of the preceding days was an *assembly* bug — a carry clobbered by
`CPY`, a branch out of range, a scratch register clobbered across a call, a frame
offset off by one — the class a compiler removes. Chris asked whether it had to be
assembly.

**The facts, verified:** no native-mode 65816 C compiler is open source today.
`cc65`'s C is 6502-only (8-bit, emulation mode — it would undo native mode).
LLVM-MOS has 24-bit addressing for the 65816 but 16-bit register mode is still an
open issue. Rust has no 65816 backend. The compilers that do emit native-mode code:
**Calypsi** (Linux-native, gratis, closed), **WDC's** (Windows, closed), and
**ORCA/C** — the historic IIgs compiler, gratis, source-available on GitHub with
`long long` since 2.2.0 and C23 features in 2.3, under a licence that permits
private use, forking and modification but **not redistribution**. Golden Gate, the
layer that runs ORCA tools on modern hosts, is $10 and closed. GNO/ME, a shell
environment ORCA/C runs under, is freeware with source.

**Decision:** ORCA/C, run by **our own Golden-Gate-style harness**: `a2min`'s
native-mode 65816 core plus a host implementation of the GS/OS file calls, the
ORCA shell calls and the Toolbox subset that the compiler, linker and shell
utilities use, with host directories mapped as the ORCA filesystem. This is the
freest option that exists: ORCA/C's official release is fetched at build time for
our own use (nothing is redistributed), the harness is ours, and — unlike running
GS/OS under MAME — it needs **no ROM and no GS/OS**, so it builds in CI like
anything else, fast and deterministically. GNO/ME under headless MAME (with the
ROM and GS/OS as pinned fetches) is the fallback if ORCA/C leans on more of the
system than its documentation suggests. Calypsi was declined to stay free of
closed binaries; the fully-free-software ideal is not available today.

**What carries over untouched:** BYTECODE.md, HEAP.md, the generator, the corpus,
the fixtures, the GC-stress mode and the scoreboard — they verify the port. The
dispatch/force/card-I/O core may remain inline assembly.

**Follow-up recorded:** a *black-box re-implementation of a FOSS native-mode 65816
C compiler* (or landing 16-bit mode in LLVM-MOS) is the way to remove the last
non-free tool. It is a project of its own; STATUS.md tracks it.

The pipeline itself is specified in [TOOLCHAIN.md](TOOLCHAIN.md).

---

## D19 — Build the instrument before the third hypothesis

**Forced by wasting an afternoon.** `eval-okay-foldlStrict` (a fold over
`range 1 1000`) stopped halting. Four hypotheses were proposed and tested by
running the simulator and watching the wall clock:

1. the window ring re-copying a mature list — *tested, "disproved"*, and the
   disproof was wrong: a stale 1 GHz-budget run from an earlier probe was still
   competing for the CPU and made the control look slow too;
2. the copy-back collector — *tested*, produced only timeouts, because bounding
   a run with `timeout` kills the simulator before it prints anything;
3. the `imports[]` root scan, which had just been widened from 128 to 256
   entries — *fixed*, and changed the instruction count by 0.2%;
4. the write barrier running a full collection per store — *arithmetic that
   matched, then didn't*.

Each experiment took minutes. The instrument that answered all four took about
fifteen to build and one run to read: **phase counters** in the VM (collections,
barrier-triggered collections, bytes promoted, window copies, forces) and a
**PC-page profiler** in a2min (`A2MIN_PROFILE=1`). The first run with both said
"51 of 52 collections came from the write barrier" and the question was over.

Two rules from it, both now in the harness rather than in anyone's memory:

- **Bound a long run with `A2MIN_BUDGET`, never `timeout`.** A budgeted run
  prints its counters, its DIAG block and its profile; a killed one prints
  nothing, which is how hypothesis 2 produced three timeouts and no data.
- **Check for a live `a2min` before measuring.** Simulated cycles are immune to
  contention but wall clock is not, and hypothesis 1 was disproved on a
  contended control.

The general form: this project already had exactly one good instrument, the bus
access counter, and it is what settled all three earlier surprises (D5's verb
gate, `LOOKUP`'s baseline, field-wise versus bulk reads). Everything else was a
single undifferentiated cycle count. **When a cost is a mystery, the cheapest
next step is usually a counter, not an experiment.**

---

## D20 — A remembered set for the write barrier; the "never points young" rule weakens

**Decided with Chris (2026-09-23), forced by a measured O(n²).**

**Context.** D6's promotion came with an absolute rule, stated in
[ARCHITECTURE.md](ARCHITECTURE.md) (rule 1) and [CARD-ISA.md](CARD-ISA.md): *a
mature object never points into the nursery; anything stored into a mature
object is promoted first; there is no remembered set.* The VM implemented it
literally, and promoting a value has exactly one mechanism, a whole
collection. So every store of a young value into a mature object cost O(live).
`eval-okay-foldlStrict` (`foldl'` over `range 1 1000`) made that quadratic: 52
collections at `n` = 550, 51 of them from the barrier, and at `n` = 1000 it
never finished, where with no card it took 3 collections (HEAP.md "the barrier
makes a promoting workload quadratic"). The card made the program quadratic
*because it works*: with no card, nothing is ever mature.

**Decision.** A store into a mature object writes the young pointer through the
bus and records (object, offset) in a bounded **remembered set** (256 entries,
the spare 4 KB of bank 5). The next collection treats each recorded field as a
root: it reads the field back, evacuates what it points at, and after promotion
writes the card address over it. A store is O(1); collections happen for memory
pressure only. A full set collects, which is exactly the old behaviour, bounded.

**The rule, restated:** *a mature object never points into the nursery except
through a recorded field, which the card must not trace.* That is sound because
nothing card-side follows a pointer the VM gives it today: the VM issues
`DATA`, `RESERVE`, `LOOKUP` (which hands back a value pointer as opaque bits),
`MEMCMP` (on names, which are never stored after creation) and `ALU64`. Two
verbs *do* chase pointers card-side — `FOLLOW` and `MARK` — and neither is
issued. **The contract for whoever issues the first one: the remembered set is
empty at that moment.** A nursery collection empties it, so "collect, then
`MARK`" is enough, and the card needs no knowledge of the set — which matters
because teaching the card to skip recorded fields would be the card making a
policy decision. This joins the `THUNK` word-0 discrepancy and the `aux2` mark
as what slice 5's card-side `MARK` must resolve before it runs (HEAP.md).

**What the measurement said afterwards, including what the diagnosis got
wrong.** With the set in place, foldlStrict's collections fell to memory
pressure: `n` = 550 went from 52 collections (38.6M cycles) to 1 (20.2M);
`n` = 650 from not finishing to 1 collection (32.5M); `n` = 1000 finishes, with
1 collection. But the counter of recorded fields read **0** on all of them: the
set was never used. The old barrier collected whenever the *value* was young,
and forcing a thunk to an `INT`, `BOOL` or `NULL` copies the value into the
thunk — no pointer crosses the bus at all. So most of the quadratic was the
barrier firing on stores that carried no pointer, and the fix that removed it is
the barrier testing *what is written* rather than what the value is. The set
still earns its place — a thunk forced to a list, a string or a closure does
store a young pointer, and `tests/fixtures/eval-okay-remembered-set` is built
to do exactly that, hundreds of times across several collections — but the
HEAP.md account of the mechanism was half right.

**Rejected:**
- *Keep promote-on-store and make collection cheaper.* It stays O(live) per
  store whatever the constant.
- *Card marking* (a dirty bit per card page, scanned at collection). It needs
  either a card-side table the CPU reads back — bus traffic proportional to the
  heap — or the card deciding what is dirty. The set is O(stores) and lives
  entirely in CPU RAM.
- *An unbounded set.* A 64 KB nursery bounds how many young objects exist, but
  not how many times they are stored; bounded plus "full means collect" keeps
  the worst case where it was before.
- *Deduplicating entries.* A search per store costs more than the rare
  duplicate; evacuation is idempotent, so a field recorded twice is harmless.

**Tests, and what writing them taught.** GC stress, the collector test every
fixture already ran under, **does not test the set at all**. Promoting at every
boundary makes every value mature before it can be stored, so a stress run
records nothing, and the first version of the mutation runner confirmed it:
under stress every mutation survived. What exercises the set is promotion
that leaves some values young, so there are now three ways to get it:

- `tests/fixtures/eval-okay-remembered-set` fills the nursery on purpose between
  building two lists of unforced thunks and forcing them (47 fields recorded
  in a normal run);
- `A2MIN_RSET_LIMIT=1` shrinks the set to one entry, so the full-set path (a
  collection mid-instruction) runs: 18 of that fixture's 20 collections;
- `A2MIN_GC_PERIOD=k` makes GC stress collect at every *k*-th boundary rather
  than every one, which puts collections between a `let`'s `ENV_NEW` and its
  `ENV_SET`s. The fixture alone never reaches that `store_field` path, and the
  "store not recorded" mutation survived until this mode existed.

**Its first run found a bug older than the set.** At period 2,
`eval-okay-deep-equality` answered `{ } == { }` and `[ ] == [ ]` *false*. The C
port allocated an empty list or set as 4 bytes, against HEAP.md's normative
8-byte minimum, so the collector wrote the empty object's forwarding pointer
over the header of whatever came next. Whether that mattered depended on which
object was next and when the collection landed, so stress at every boundary
never hit it, and periods 2, 5 and 7 did. Fixed in `list_alloc`/`attrs_alloc`
and `obj_size`, and `scan_one` now steps by `obj_size` instead of keeping its
own copy of every size, which is how the two had been able to disagree.

The scoreboard check runs every fixture normally, at stress periods 1 and 3,
and with a one-entry set. `scripts/mutation-vm.sh` (`checks.mutation-vm`)
breaks the set six ways and the object sizes two ways, and requires the
fixtures to notice each (D14). It mutates a copy of the sources, never the
tree: its first version edited `target/vm/c` in place, and a low-memory kill
(which skips any trap) twice left a mutation behind.

---

## D21 — L6 begins with strings, not NARHASH

**Decided with Chris (2026-09-24).** The plan was "L6 starts with `NARHASH`,
priced first on real programs". But `NARHASH` hashes a **directory tree held on
the card**, and nothing can put a file tree there until GS/OS file I/O exists
(L7): there is no real program to price it on, only synthetic trees built for
the purpose. Meanwhile the first M4 target, a `.drv` whose hash matches the
host's, needs **no files at all**. A derivation with a builder, arguments and an
environment but no source paths needs:

1. SHA-256 of strings: `builtins.hashString "sha256"`, on the card's proven
   `HASH_PUSH`/`HASH_STREAM` + `HASH_FINAL`;
2. ATerm serialisation of the derivation;
3. the `derivation` primop: `hashDerivationModulo`, then `STOREPATH`, the verb
   M1 proved byte-identical to Nix;
4. string context, so a derivation's inputs are recorded.

**Decision:** that order. `NARHASH` arrives with file I/O, where it is first
needed and where real trees exist to price it on.

**Rejected:** designing a card-side tree representation now and pricing
`NARHASH` on synthetic trees. The representation would be guessed before the
thing that fills it (the file-system layer) exists, and the D5 gate would be
answered on inputs no program produces.

## D22 — `derivation` is Nix's own `derivation.nix`; one per program until context

**Decided in the L6 loop (2026-09-25).** Nix implements `derivation` as a Nix
file wrapped around the `derivationStrict` primop. The VM does the same: the
generator embeds that file verbatim and compiles it like an import
(BYTECODE.md "Imports"); only `derivationStrict` is C (DERIVATION.md).

Why not all C: the wrapper's result (`all`, `drvAttrs`, one attribute set per
output, `outputName`, its laziness, so `d.name` never builds anything) is
observable, and reproducing it by hand is a place to be subtly wrong. Compiled
from Nix's own source it is right by construction, and it costs one chunk.

This does not bend D7. The embedded file computes nothing: no hash, no path,
no answer is precomputed on the host, and `checks.fixture-purity` still finds
no hashing code in the generator. On the target it is the same bytecode any
program's `let` would be, and at L5 the on-target compiler compiles the same
source.

**Interim, until string context:** a second `derivationStrict` in one program
is `E_UNSUP`. Without context, one derivation's output path reaches another's
attributes as plain text: the second `.drv` has no `inputDrvs` and a
well-formed, wrong path. One derivation per program cannot consume another.

**Rejected:** a context *marker* on derivation outputs, propagated through
every string operation and refused by `derivationStrict`. It touches every
site that real context propagation will touch, to be rewritten there
immediately after; the guard is three lines and goes when context lands.

**Superseded the next step (string context):** the guard is gone. A second
derivation now reads the first's output as an input, and the one context it
cannot honour -- a `drvPath` in a string, which needs the `.drv`'s closure --
is `E_UNSUP` instead.

## D23 — `builtins.currentSystem` is `"w65-gsos"`

**Chosen on the trajectory Chris set (2026-10-26)** -- asked what fits it best,
rather than given a value: the aim is a nixpkgs that supports this machine as
a platform, and a nixpkgs hosted for it. So the IIgs reports what it is. Easy
to revisit (below).

- **`w65`** is GNU `config.sub`'s CPU name for WDC's 65xx line (it maps `w65`
  to vendor `wdc`), so a nixpkgs `hostPlatform.config` of `w65-unknown-gsos`
  is half-understood by GNU tools already. nixpkgs has no 65xx CPU today;
  adding one to `lib/systems/parse.nix` is five lines, like its `msp430` and
  `avr` entries, and **`gsos`** a kernel, like its `ghcjs` or `mmixware`.
- **Honest semantics:** a derivation with `system = builtins.currentSystem`
  asks for a builder of the current system. Reporting `x86_64-linux` would
  make every derivation the IIgs instantiates claim to need a Linux machine
  to build it.
- **Host parity is kept:** `nix-instantiate --option system w65-gsos` makes
  the host report the same value, so fixtures that use it are still the
  host's own answers (D7). The corpus tests that wait on `currentSystem` use
  it only as a derivation's `system`, never print it.

**Rejected:** `x86_64-linux` (parity with no flag, at the price of every
`.drv` lying about its builder); a new CPU name such as `w65816` (no tool
knows it; `w65` is already GNU's). Where it lives: the generator emits it as
a string constant, because a primop is a curried lambda and this is a value;
at L5 the on-target compiler does the same. It is one constant
(`tools/nbc/src/opcodes.rs BUILTIN_VALUES`) and a regenerated fixture to
change.

## D24 — `builtins.nixVersion` is `"2.34.9"`, `langVersion` 6

**Chosen as D23 was (2026-10-26), easy to revisit.** The VM evaluates Nix as
2.34.9 does, rule for rule -- every fixture is that version's answer (D7) --
so it says so. nixpkgs reads `nixVersion` only to refuse evaluators older
than it supports (`lib.versionAtLeast builtins.nixVersion ...`), which this
one is not, and tvix answers `"2.3-compat"` for the same reason (its
`eval-okay-nix-version-cmp` asks only that the version be past 2.18).
`langVersion` 6 is 2.34's. If the VM ever claims a different Nix's semantics,
both move with it; the generator emits them as constants (`opcodes.rs`
`BUILTIN_VALUES`).

## D25 — Regular expressions are a port of libstdc++'s, on a bank of their own

**Decided 2026-10-26, from the measurements in REGEX.md.** `builtins.match`
and `split` answer what libstdc++'s `std::regex` answers, and that is not
what a POSIX matcher answers. Its depth-first search keeps the first whole
match it meets, never tries a loop's earlier exits once the loop has
matched, enters a loop twice at one place, keeps captures across
iterations, and refuses or accepts syntax by its own rules (strict escapes,
signed ranges). Any engine written to a specification, POSIX's or RE2's,
gives other answers to ordinary patterns, `(a*)(ab)?` among them. So
`regex.c` ports libstdc++ structure for structure, and is checked against
it, natively, case by case, and on the VM against the host. The licence
allows it: GPL-3.0-or-later with the GCC exception, inside a GPL-3.0-or-later
project. It inherits libstdc++'s exponential cases, and those hang Nix too.

Its working memory is **bank 7**, which nothing else uses and no collection
touches. The NFA and the matches survive the reserve that builds the result,
so the result needs no second search. omf2bin skips the bank for code, as it
skips bank 5, and bank-gate.py checks it. The alternative, the collector's
stage (bank 2) between collections, would have had to search twice, once to
size the result and once to build it after the reserve. Like the other fixed
banks, this one becomes a Memory Manager handle when GS/OS loads the VM (L7).

## D26 — What needs GS/OS is refused at run time, by name

**Decided with Chris (2026-10-26) as the bytecode compiler's finish line:**
"nbc refuses nothing; things needing GS/OS are refused at runtime by name
and not counted." Files, the environment, the network and the store are
GS/OS's (L7), and the VM does not run under it yet. So the fifteen builtins
that need them -- `fetchGit`, `fetchMercurial`, `fetchTarball`,
`fetchTree`, `fetchurl`, `filterSource`, `getEnv`, `getFlake`, `hashFile`,
`path`, `pathExists`, `readDir`, `readFile`, `readFileType`, `storePath` --
compile like any primop (they print as `<PRIMOP>`, are reachable as
`builtins.NAME`, as `__NAME`, and the fetchers unqualified, as in Nix) and,
applied, force their arguments in order and stop with `E_GSOS` and their
name. Forcing first keeps Nix's order: a throw in an argument is still
caught, as it is by the host before any file is touched. The scoreboard
counts these in a bucket of their own, `need GS/OS`, which no change to the
generator can move and L7 will.
