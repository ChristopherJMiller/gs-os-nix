# Architecture

## Division of labor

Three parties. **Only one of them decides anything.**

| Party | Owns |
|---|---|
| **Apple IIgs (65816)** | Lexer, parser, compiler, bytecode VM, lazy forcing, all primops, string context, store policy, derivation logic, CLI, GS/OS integration |
| **Card (ECP5 FPGA)** | Memory capacity, compute-bound kernels, coarse amortizing verbs. Never decides, never touches the world |
| **Linux host** | Toolchain, golden test fixtures, MAME CI. **Nothing at runtime** |

If the card ever makes a policy decision or performs I/O, the Apple II has become a
terminal and the project has failed. That is the single invariant to protect.

## The economic fact everything rests on

Two verified numbers (see [REFERENCES.md](REFERENCES.md)):

- The IIgs **throttles to 1.024 MHz** for any `$C000–$CFFF` access — the FPI stretches
  the clock to sync with the Mega II. One card byte ≈ **~10–11 fast-cycle equivalents**.
- `LDA [dp],Y` (direct-page indirect **long** indexed, `B7`) = **6 cycles**, +1 if the
  direct-page low byte ≠ 0, +1 on page cross → **6–8 cycles** per dereference, before
  the cost of getting a 3-byte pointer into direct page at all.

> **One card access costs about the same as one fast-RAM pointer dereference.**

Consequences, and they are not negotiable:

1. **The bus carries verbs, not data.** Offload chains, searches, and high-volume
   streaming. Never offload a single dereference.
2. **The hot heap cannot live on the card.** This kills the naive "card is memory
   behind a window" design.
3. Every verb must earn its crossing by replacing *many* CPU cycles. Enforced by
   `checks.verb-economics` — a verb that doesn't pay gets deleted.

## Memory model

The generational boundary **is the slot connector**. This is the project's keystone
idea and the reason the bus penalty stops mattering.

```
      Apple IIgs fast RAM                 Card SDRAM
   ┌──────────────────────────┐        ┌──────────────────────────┐
   │ NURSERY                  │        │ MATURE SPACE             │
   │  thunks born here        │ promote│  survivors only          │
   │  deref = 6-8 cycles      ├───────>│  O(survivors)            │
   │  CPU copying collector   │  on    │  mark-compact (card-side)│
   │  most die here, never    │ survival                          │
   │  crossing the bus        │        ├──────────────────────────┤
   ├──────────────────────────┤        │ STORE (immutable)        │
   │ VM value + env stacks    │        │  pure BRAM read cache,   │
   │  hottest, youngest data  │        │  no coherency protocol   │
   └──────────────────────────┘        └──────────────────────────┘
```

Lazy functional evaluation has extreme infant mortality, so the overwhelming majority
of allocations never cross the bus. Only data that has *proven* it will live pays the
crossing. This is Symbolics' **ephemeral** GC (Moon 1984) with the generation boundary
placed on a peripheral bus.

**Measured** (`just bench-nursery`, `target/heap/nursery.s` on the 65816 harness): a
workload of 50 objects / 600 bytes born in the nursery, with 4 objects / 48 bytes kept
live by two roots. The collection ran in CPU RAM; promotion was **one `DATA` burst of
48 bytes into a `RESERVE`d block — 66 bus accesses in total, 18 of them fixed
overhead**. 552 bytes of garbage never touched the slot. The fraction that crosses is
set by the workload's survival rate, not by the mechanism; the mechanism's cost is
~1 access per surviving byte.

Three rules make this work, and they are load-bearing:

1. **A mature object never points into the nursery.** The card cannot dereference a
   CPU-RAM address, so an old→young pointer would be meaningless to it anyway.
   Anything stored into a mature object is promoted first. That *is* the write
   barrier; there is no remembered set.
2. **Young pointers carry bit 31.** The card's 1 MB heap never uses it, so the two
   address spaces cannot be confused.
3. **Promotion translates by a constant.** Cheney lays survivors out contiguously
   with sizes preserved, so `card = base + (addr − staging_base)` — no forwarding
   table on the promotion path.

Supporting decisions:

- **Tags: the bank byte is the type tag** CPU-side (region tagging — the classic
  6502-Lisp trick). Free type dispatch on a machine that is bad at masking. The card
  uses explicit tag bits.
- **Cache: 4-way set-associative, tree-PLRU (3 bits/set)** over SDRAM — *not* true LRU,
  which costs gates for negligible gain. For pointer chasing, associativity beats
  policy precision.
- **Store region needs no invalidation.** It is immutable and content-addressed, so it
  is a pure read cache: no dirty bits, no coherency, cacheable forever.
- **Inline caches at bytecode sites** (monomorphic → polymorphic) eliminate repeat card
  round-trips entirely. Cheapest large win available, and it lives in software.

## One source of truth for the card

The RTL is authoritative. Three consumers, never divergent:

```
rtl/*.v ──┬── verilator ─────> C++ class ──> MAME a2bus device
          │                                  (real GS/OS boots the actual RTL)
          ├── cocotb ────────> unit tests (NIST vectors, host-nix parity)
          └── yosys/nextpnr/trellis ──> ECP5 bitstream
```

MAME's `device_a2bus_card_interface` maps ~1:1 onto a Verilog port list
(`read_c0nx`/`write_c0nx`, `read_cnxx`, `read_c800`/`take_c800`, `bus_reset`,
`raise_slot_irq`/`raise_slot_dma`), which is why this works cleanly rather than
needing an abstraction layer. Do not invent one.

## Software stack on the 65816

```
  target/lex/     lexer, driving the card's DFASTEP verb
  target/parse/   recursive-descent parser + compiler -> bytecode
  target/vm/      bytecode VM: thunks, lazy forcing, attrsets, lists, strings
  target/heap/    nursery, copying collector, promotion to card
  target/primops/ all primops, incl. STRING CONTEXT tracking
  target/store/   store paths, NAR, ATerm, derivation identity
```

The bytecode the VM runs is defined in [BYTECODE.md](BYTECODE.md): tvix's shape
(stack VM, constant pool and code per chunk, explicit thunk placement, in-place
update) with two 65816 adjustments recorded in [DECISIONS.md](DECISIONS.md) D17.
Until the on-target compiler exists (L5), `tools/nbc` on the host produces `.nbc`
**test fixtures** from `.nix` sources — test-only by construction (D7,
`checks.fixture-purity`), Rust by D16 — and `scripts/scoreboard.py` runs the
vendored tvix corpus (`tests/corpus/tvix/`) through it and the VM. That count is
the headline metric.

**String context must stay here.** Nix tracks which store paths a string value depends
on, so `"${pkgs.hello}/bin/hello"` makes the referring derivation depend on `hello`.
Contexts flow out of derivation outputs at *runtime*, so this can never be a
compile-time concern. It is the part naive Nix reimplementations get wrong.

## Hardware target

**ECP5** (25F ≈ 24k LUTs, 85F ≈ 84k). The heap needs real SDRAM/DDR3; ice40's 128 KB
SPRAM cannot hold a Nix heap.

Gate budget. **Measured** rows come from `just area` (`synth_ecp5 -noabc9`); keep them
current as modules land, since a budget that drifts from reality is worse than none.

| Block | LUT4 | FF | Status |
|---|---:|---:|---|
| `sha256_core` (compression) | 1688 | 1064 | measured |
| `sha256` (+ padding FSM) | 1673 | 1657 | measured |
| `storepath` (fold + nixbase32) | 505 | 0 | measured — purely combinational |
| `keycam` (32-wide CAM) | 1400 | 0 | measured — combinational |
| `alu64` (sequential mul/div) | 1462 | 336 | measured — the ~1.5k estimate held |
| Cheney collector (inside the card's engine) | ~800 | ~380 | measured by difference |
| **`a2nix_card`** (whole card, incl. above) | **11213** | **6052** | measured, L3 complete |

The whole card is **11213 LUT4 of a 25F's ~24k (47%)**. The CAM is the expensive
addition: 1400 LUT4 of comparators plus **1024 flops** for the 32×32-bit lane array,
which is most of the FF growth. That array cannot be block RAM — comparing all lanes
in one cycle is the entire point — so it is the honest price of O(1) lookup.

The 32-entry input FIFO mapped to 4 `TRELLIS_DPR16X4` distributed-RAM blocks rather
than flops, which is what we want.

The **heap itself is not in this budget** — it is off-chip by design (see
[HEAP.md](HEAP.md)), because a 25F's ~128 KB of block RAM cannot hold a Nix heap.

**Known inefficiency:** of the card's 5333 flops, 2048 are the eight 256-bit parameter
slots. Those are addressed one byte at a time and never need random access, so they
belong in block RAM. Worth fixing if flops get tight — not before.

Toolchain is fully open and already packaged: `yosys`, `nextpnr`, `trellis`,
`openfpgaloader`, `verible`. Refresh these numbers with `just area`.

## Scope boundaries

- `nix build nixpkgs#hello` is **permanently** out of reach. Evaluating nixpkgs needs
  gigabytes of heap. This is physics, not effort.
- Self-contained flakes **evaluate**. Flake *fetching* is reachable via **HTTP +
  Ed25519**, not TLS: narinfo files are Ed25519-signed and store paths are
  content-addressed, so Nix's integrity model never needed TLS. Marinetti (pure 65c816
  TCP/IP, with an existing Uthernet II link layer) covers transport.
- Pre-generated `.nbc` bytecode files are **test fixtures only**, never a runtime
  component. Enforced by `checks.fixture-purity`.
- Self-hosting the compiler is on the **critical path** (L5), not a late aspiration.
