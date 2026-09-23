# Card ISA (normative)

This document is the **contract** between `rtl/a2nix_card.v` and `target/`. Both sides
must be changed together, and `tb/test_isa.py` asserts the register map matches.

Slot `n` decodes at `$C0(n+8)0`–`$C0(n+8)F` (slot 4 → `$C0C0`, slot 5 → `$C0D0`, …),
plus the 2 KB shared expansion window `$C800–$CFFF` arbitrated via `take_c800`.

No slot ROM at `$CnXX` before L7. The card is not a boot device, so software drives
`$C0nX` directly. This deliberately avoids reimplementing storage-card firmware.

## Register map

16 bytes is tight, so the map uses **indexed windows**: select a slot, then stream
bytes through a port that auto-increments.

| Addr | Name | Access | Meaning |
|---|---|---|---|
| `$C0n0` | `CMD` | W | Write a verb opcode; execution begins immediately |
| `$C0n0` | `STATUS` | R | `b7` BUSY, `b6` DONE, `b5` ERR, `b4` **READY**, `b3..b0` error code |
| `$C0n1` | `CTRL` | W | `b0` RESET, `b1` IRQ_EN, `b2` ABORT, `b3` **RESET_PORTS** |
| `$C0n1` | `ID` | R | `$A2` magic, then version via `PPORT` |
| `$C0n2` | `ASEL` | W | Select address register `A0..A3`; resets its byte index |
| `$C0n3` | `APORT` | RW | Sequential bytes of selected address register (LSB first, 4 bytes); auto-increments byte index |
| `$C0n4` | `DATA` | RW | Heap/store data at selected address register; **auto-increments that register** |
| `$C0n5` | `TAG` | R | The byte at the selected address register, **without advancing it** |
| `$C0n6` | `PSEL` | W | Select parameter/result slot `P0..P7`; resets its byte index |
| `$C0n7` | `PPORT` | RW | Sequential bytes of selected `P` slot; auto-increments |
| `$C0n8` | `LEN` | RW | Sequential bytes of the 32-bit length/count register (LSB first) |
| `$C0n9` | `IRQ` | RW | Read: pending interrupt cause. Write: acknowledge |
| `$C0nA`–`$C0nF` | — | — | Reserved. Reads return `$00` |

**Address registers** `A0..A3` are 32-bit. They are the pointer registers the 65816
does not have, and `DATA`'s auto-increment is what makes sequential heap access cheap.

**Parameter slots** `P0..P7` are 32 bytes each — sized to hold a SHA-256 digest or a
32-character nixbase32 store-path component.

### Byte order — the two ports differ, deliberately

| Port | Order | Because |
|---|---|---|
| `APORT`, `LEN` | **little-endian integer**, LSB first | They hold numbers, and the 65816 is little-endian, so a pointer streams out in native order |
| `PPORT` | **byte array**, index 0 = first byte | They hold digests and path strings, where "byte 0" means the first byte, not the least significant |

Getting this backwards yields a byte-reversed digest — which still *looks* like a
plausible hash, so it is worth stating explicitly. `tb`/`card_main.cpp` assert both
orders.

### Rewinding a port index — `CTRL` bit 3

`APORT` and `PPORT` rewind whenever `ASEL`/`PSEL` is written. **`LEN` has no select
register**, so before this existed, a caller that read a *partial* `LEN` left its index
rotated permanently, and every later 4-byte write landed shifted — silently, with the
card streaming a wrong length. `CTRL` bit 3 rewinds `aidx`, `pidx` and `lenidx`
together.

**Write `CTRL <- $08` before setting `LEN`** unless you are certain the index is at
zero. The cost is one bus access.

### `DATA` and `TAG` read the same byte

Both return the byte at the selected address register; only the side effect differs.
`DATA` advances the register (and prefetches the next byte, which is what makes
sequential heap walking cheap); `TAG` does not. So `TAG` answers "what type is the
object `A_n` points at?" without disturbing the pointer — exactly what type dispatch
wants.

Reads answer immediately because the card keeps the byte at `areg[asel]` prefetched,
re-issuing whenever `ASEL` is written, the last `APORT` byte lands, or `DATA` is
accessed.

## Verbs

Written to `CMD`. All set BUSY, then DONE or ERR. Operands are named by the register
they read.

### Hashing and store paths

| Op | Name | In | Out | Layer |
|---|---|---|---|---|
| `$10` | `HASH_INIT` | — | resets streaming hash state | L1 |
| `$15` | `HASH_PUSH` | enters push mode; writes to `DATA` are absorbed | — | L1 |
| `$11` | `HASH_STREAM` | `A0` = start, `LEN` = bytes | absorbs card-resident bytes | **L3** |
| `$12` | `HASH_FINAL` | — | `P0` = digest, **`P3` = 32-char nixbase32** | L1 |
| `$13` | `NARHASH` | `A0` = tree root node | `P0` = digest of the NAR serialization | L6 |
| `$14` | `STOREPATH` | `P0` = digest supplied by the caller | `P3` = 32-char nixbase32 | L1 |

`HASH_PUSH` exists because M2 must work before the card has any memory of its own
(that arrives at L3), so the CPU has to hand bytes over directly. It stays useful
after L3 for data the CPU holds in main RAM and has no reason to park on the card.

> ### Measured economics
>
> `card_main.cpp` counts real bus accesses, modelling the card at **48 cycles per
> `$C0nX` access** (a 50 MHz card against the 1.024 MHz throttled slot). That ratio is
> not a detail: it decides how much of a compression stall the CPU ever observes.
>
> | | accesses/byte | ≈ cycles/byte | vs software |
> |---|---:|---:|---:|
> | software SHA-256 on 65816 | — | ~3125 | 1x |
> | `HASH_PUSH`, poll per byte | 2.00 | ~20 | ~155x |
> | `HASH_PUSH`, 16-byte bursts | **1.07** | ~11 | **~285x** |
> | `HASH_STREAM`, 108 bytes | **0.51** | ~5 | **~600x** |
>
> `HASH_STREAM` costs a **fixed ~55 accesses regardless of length** — only the
> pointer, the count and the verb cross the bus, never the data. So its advantage
> grows with message size: 0.51 accesses/byte at 108 bytes, ~0.05 at 1 KB. This is the
> principle `NARHASH` takes to its conclusion, and the reason the heap lives on the
> card at all.
>
> So the FIFO is worth **1.87x** on the streaming phase — real, but nothing like an
> order of magnitude, because at this clock ratio the hasher drains ~24 bytes per bus
> access and is simply never the bottleneck. The ~100-300x hashing claim in
> ARCHITECTURE.md holds.
>
> **Fixed overhead is now the thing to attack: ~72 accesses per call**, almost all of
> it reading results back 32 bytes at a time through `PPORT`. A store path only needs
> `P3` (33 accesses); the testbench also reads `P0` for diagnostics. End-to-end for a
> ~91-byte fingerprint that is ~5.4 accesses/byte, dominated by readout rather than
> hashing. Worth revisiting if store-path computation ever becomes hot.
>
> #### Methodology note, learned the hard way
>
> The first version of this harness advanced the card **one** cycle per bus access,
> making it 48x slower than reality. That inflated poll counts to 13.7 accesses/byte
> and produced a confident, wrong conclusion that the card was a ~23x accelerator.
> **Any verb-economics measurement must model the clock ratio**, or it measures the
> harness instead of the hardware.

`HASH_FINAL` yields the encoded path in `P3` as well as the raw digest in `P0`,
because `storepath.v` is purely combinational — once the digest settles, the 32
characters cost nothing. `$14 STOREPATH` remains for folding a digest the caller
computed some other way.

### Flow control in push mode

A 32-entry input FIFO sits between `DATA` and the hasher, so `STATUS` bit 4 (READY)
means **"at least 16 FIFO slots are free"** — one poll licenses a burst of 16 writes:

```
  CMD <- $10 (HASH_INIT)                 ; mandatory: abandons any partial message
  CMD <- $15 (HASH_PUSH)
  while bytes remain:
      repeat: b = STATUS  until b & $10  ; READY: >= 16 slots free
      write up to 16 bytes to DATA
  CMD <- $12 (HASH_FINAL)                ; drains the FIFO, then pads
  repeat: b = STATUS  until b & $40      ; DONE
  PSEL <- 3 ; read 32 bytes from PPORT   ; the store path
```

Overrunning the FIFO raises `ERR`/`OVERRUN` rather than dropping bytes — a short
message would otherwise yield a plausible-looking but wrong digest, the worst failure
mode available to a hash accelerator. At the modelled clock ratio overrun is actually
unreachable (the card drains ~24 bytes per bus access), so the guard is defence in
depth for a slower card clock.

`HASH_FINAL` waits for the FIFO to empty before padding; otherwise the message would
be silently truncated by however many bytes were still queued.

**`HASH_INIT` is required before pushing** and is honoured from any state — it means
"abandon whatever you were doing". An earlier version only accepted it when idle,
which made it a no-op mid-message and let a partial message leak into the next digest.

`NARHASH` is the highest-value verb in the design: it serializes a directory tree to
NAR *and* hashes it entirely card-side. Zero bytes cross the bus; 20 come back.
Store-path computation is the highest-data-volume operation in Nix.

`STOREPATH` performs the fingerprint hash, the XOR fold to 160 bits
(`result[i % 20] ^= h[i]`), and the 32-lane nixbase32 encode — which emits **from the
end** of the byte string.

### Heap and memory

| Op | Name | In | Out |
|---|---|---|---|
| `$20` | `ALLOC` | `LEN` = size, `P0` = tag | `A1` = address of new object |
| `$25` | `RESERVE` | `LEN` = bytes | `A1` = base of a headerless block; bumps the allocator |
| `$21` | `FOLLOW` | `A0` | `A1` = first non-indirection in the chain |
| `$22` | `MEMCMP` | `A0`, `A1`, `LEN` (word-aligned) | `P0` byte 0 = sign: `$00`, `$01`, `$FF` |
| `$23` | `MEMCPY` | `A0` → `A1`, `LEN` | — |
| `$24` | `INSTANTIATE` | `A0` = template, `A1` = dest, `P0` = substitutions | wide structured heap write |
| `$40` | `MARK` | `A0` = root list, `LEN` = count | marks reachable mature objects |
| `$41` | `COMPACT` | — | O(survivors) mark-compact |

`FOLLOW` and `MEMCMP` exist because chains and searches amortize the bus crossing.
A *single* dereference must stay on the CPU.

`MEMCMP` compares a **word at a time**, which is why both addresses must be
word-aligned — every string payload in the heap is, by construction
([HEAP.md](HEAP.md)) — and an unaligned operand is `E_BADPARAM` rather than a
quiet comparison of the word that happens to contain it. The bytes are weighed
in **address order**, so the byte at `+0` is the most significant: a plain
little-endian word compare would order two names by their last byte first, which
is the sort of thing that corrupts a sorted-name merge without ever looking
wrong. The final partial word is masked down to the bytes `LEN` covers, so the
answer does not depend on the zero padding HEAP.md guarantees being present.
`LEN` = 0 is equal.

It earns its crossing twice over: one command replaces a byte-by-byte compare
over the bus, and it is what lets the CPU confirm a `LOOKUP` candidate's name
**without copying the attribute set back across the bus** — the copy that, as
measured below, is what stopped `LOOKUP` from paying.

**`MARK` and `COMPACT`** are the two halves of a Cheney semispace collection
([DECISIONS.md](DECISIONS.md) D15, layout in [HEAP.md](HEAP.md)):

- `MARK`: `A0` points at a root list of `LEN` 32-bit pointers. Each root is evacuated
  into to-space and **overwritten in place with its new address**; then everything
  reachable is copied by the scan. May be issued repeatedly — later calls append to
  the same to-space — so the CPU can hand over its stack, then its nursery's
  references, then flip.
- `COMPACT`: flips the halves. Allocation resumes immediately after the survivors,
  and the old from-space becomes the empty to-space for next time. Single cycle.

Every pointer the CPU holds into mature space must have been in some root list before
`COMPACT`, or it dangles. Pointers into to-space (already evacuated by an earlier
`MARK`) and null are passed through unchanged.

> ### Measured: a collection costs the bus almost nothing
>
> `MARK` of a 2-element list sharing one `INT` (24 live bytes, three objects traced):
> **13 bus accesses, 624 card cycles.** The 13 are the root pointer, `LEN`, the verb
> and a few `DONE` polls; no object ever crosses the bus. The 624 are the O(survivors)
> work — about 26 card cycles per live byte at one word per three cycles, plus header
> reads — and they scale with what survives, never with the size of the heap.
>
> That is the whole point of D6: the CPU's nursery decides what is worth promoting,
> and the card's collector then costs the CPU only the handful of accesses it takes
> to say "these are my roots".

`INSTANTIATE` is the one surviving idea from Reduceron: a template becomes a single
wide card-side structured write rather than word-at-a-time construction.

`RESERVE` exists for **promotion**. A nursery collection (`target/heap/nursery.s`) is
a Cheney copy inside CPU RAM into a staging area; survivors come out contiguous with
their sizes preserved, so the whole staging area can be pushed to the card as **one
`DATA` burst** into a reserved block, and every young pointer translates by a constant:
`card = base + (addr − staging_base)`. One verb and ~1 access per surviving byte, with
no per-object `ALLOC`. Young pointers carry bit 31, which the card's 1 MB heap never
uses, so the two address spaces cannot be confused.

The rule that makes this sound: **a mature object never points into the nursery.**
The card cannot dereference a CPU-RAM address, so an old→young pointer would be
meaningless to `LOOKUP` or `FOLLOW` anyway. Anything stored into a mature object is
promoted first. That is the entire generational write barrier — there is no
remembered set.

### Evaluation support

| Op | Name | In | Out |
|---|---|---|---|
| `$30` | `LOOKUP` | `A0` = attrset, `P0` = key hash (4 bytes, LSB first) | `A1` = value address, `P1` = attribute index; or ERR `NOTFOUND` |
| `$50` | `ALU64` | `P0`, `P1` operands, `P2` = op selector | `P3` = 64-bit result |
| `$60` | `DFASTEP` | `A0` = text, `P0` = DFA state | `P1` = token type + length |

`LOOKUP` uses a 32-wide key-hash CAM, collapsing attrset lookup from O(log n) pointer
chases plus string compares to O(1) per batch. This is *the* hot loop in Nix
evaluation.

`P1` returns the **attribute index**, not just the value, because a hash match is only
a **candidate** — 32-bit hashes collide, so the caller must compare the name
(docs/HEAP.md). Without the index it could not locate the name to compare. On a
collision the CAM returns the **lowest** matching lane, which is the one a sorted-order
search would have found.

> ### Measured: LOOKUP is 2.7x cheaper than a CPU binary search
>
> Over 100 sorted attributes: **23 bus accesses** for `LOOKUP` against **63** for the
> 65816 doing a binary search through the `DATA` port.
>
> It was 1.5x (43 accesses) at a byte-wide memory port, because filling a CAM batch
> cost 32 × 4 bytes × 2 cycles ≈ 256 card cycles and the CPU saw that as ~27 `DONE`
> polls -- the one-cycle comparison was lost in the noise, and the CAM's 1400 LUT4 and
> 1024 flops were buying almost nothing. Widening the port to 32 bits made a lane one
> access (docs/HEAP.md), and cost no area: the byte-assembly registers the engine
> needed simply disappeared.

> ### Measured again, on real programs: LOOKUP does not pay below ~70 attributes
>
> The 2.7x above is against a 65816 **binary search through the `DATA` port** — one
> bus access per comparison. That is not the implementation the VM ended up with.
> `deref` copies a mature object into a window in one burst and the VM then scans
> it **in RAM at full speed** (VM.md, HEAP.md), so the CPU path pays for the set
> once and nothing per comparison. Measured against *that*
> (`checks.verb-economics`, a promotion at every bytecode boundary so the set is
> mature), selecting the last of `n` attributes:
>
> | `n` | LOOKUP | CPU scan of the copy | LOOKUP's own bus cost |
> |---:|---:|---:|---:|
> | 8 | 372,515 | 366,617 | +20 |
> | 32 | 1,095,831 | 1,092,285 | +20 |
> | 64 | 2,053,367 | 2,052,957 | +20 |
> | 128 | 3,968,439 | 3,974,301 | +20 |
>
> The crossover is between 64 and 128. The verb's own cost is a flat **20 bus
> accesses** — set `A0`, four hash bytes, the command, the poll, `A1`, the index —
> no matter how large the set, which is exactly the property the CAM was built for;
> what changes with `n` is only the scan it replaces. On the corpus as it stands
> the verb is a small net **loss** (-1.7% of cycles over ten fixtures), because
> Nix's own test programs have attribute sets of two to five.
>
> That is not an argument for deleting it, and it is not a correction to the
> hardware: 20 accesses for any `n` is the right shape. It says the *software*
> gives the verb no room. LOOKUP still has to hand the set back to the CPU so the
> name can be confirmed against a hash candidate (docs/HEAP.md), which means a
> whole-object copy the CPU scan would have paid for anyway — so the verb saves
> the comparisons and keeps the copy. The way to make it pay at the sizes real
> code uses is to confirm the name **on the card**, with the `MEMCMP` verb that is
> already in this table and that the VM does not yet call; then a hit costs ~24
> accesses and no copy at all. Recorded here rather than acted on, because it is a
> slice-5 change to how mature sets are read (STATUS.md item 5).

`ALU64` exists because the 65816 has **no multiply or divide at all** and Nix integers
are 64-bit. Operands `P0`/`P1` and the result `P3` are 8-byte little-endian integers in
byte-array slots (byte 0 is the LSB); `P2` byte 0 selects the operation:

| Op | Name | Cycles | Notes |
|---|---|---:|---|
| `$0` | `ADD` | 1 | wrapping |
| `$1` | `SUB` | 1 | wrapping |
| `$2` | `MUL` | 64 | low 64 bits; the same signed or unsigned |
| `$3` | `DIV` | 64 | **truncates toward zero**, as Nix and C do. `x/0` → ERR `BADPARAM` |
| `$4` | `MOD` | 64 | remainder takes the **dividend's** sign: `-7 % 2 = -1`. `x%0` → ERR |
| `$5` | `AND` | 1 | |
| `$6` | `OR` | 1 | |
| `$7` | `XOR` | 1 | |
| `$8` | `CMP` | 1 | `-1`, `0` or `1` as a signed 64-bit result. A **true** signed compare, not a subtraction — subtracting overflows for operands of opposite sign |
| `$9` | `NEG` | 1 | two's complement; `NEG INT64_MIN = INT64_MIN` |

Multiply and divide are **sequential** (64 shift-add / restoring steps) rather than
combinational. A one-cycle 64×64 multiplier is ~4k LUT4 on an ECP5; the sequential
unit is 1462, and its 64 cycles are about one bus access of latency — which the CPU,
polling `DONE`, cannot see. `INT64_MIN / -1`, which C leaves undefined, wraps to
`INT64_MIN`.

> ### Measured: a card multiply is ~18x cheaper than the 65816's own
>
> | | cost | ≈ fast cycles |
> |---|---:|---:|
> | 65816 shift-add multiply, **16-bit native mode** (`just bench-mul64-native`) | **5693 CPU cycles** | 5693 |
> | 65816 shift-add multiply, 8-bit emulation mode (`just bench-mul64`) | 9675 CPU cycles | 9675 |
> | `ALU64 MUL`, end to end: operands in, verb, poll, result out | **32 bus accesses** | ~320 |
>
> Operands `123456789 × 987654321`; the all-ones worst case is 8560 cycles in native
> mode. The gate is judged against the **native** figure, since the L4 VM runs in
> native mode: the card wins ~18x, and ~27x at the worst case.
>
> **A prediction corrected by measurement.** The first version of this table had
> only the 8-bit number and guessed native mode would "roughly halve" it. It does
> not. Sixteen-bit registers turn each 64-bit operation into four word ops instead
> of eight byte ops, but every word op costs one more cycle for the extra byte it
> moves, so the saving is **41% (1.70x)**, not 50%. Both benchmarks live in
> `checks.a2min-native` so the numbers here can be regenerated rather than trusted.
> Two thirds of the card's 32 accesses are moving the 16 operand bytes in and 8
> result bytes out through `PPORT`; the arithmetic itself is invisible.
>
> Writing that benchmark surfaced two 6502 traps worth remembering, both of which
> `a2min` emulated faithfully: **`CPX` clobbers the carry** a multi-byte `ADC` chain
> depends on, and **`abs,X` adds X unsigned**, so the "count from `$F8`" idiom needs
> its base `$F8` *below* the target, not 8 above it.

> ### Measured again, on real programs: ALU64 is worth 0.1%
>
> 18x per multiply is right, and it buys almost nothing: over ten corpus fixtures
> (`checks.verb-economics`) masking ALU64 off and letting ORCA/C's own long-long
> helpers do the work costs **0.1% of cycles**. Nix's test programs barely
> multiply — they build attribute sets and force thunks.
>
> That is not a case for deleting the verb. `ALU64` is the one entry in this table
> that exists for **capability**, not throughput: without it the VM would need a
> software 64-bit divide anyway, and it now has one (`run.c alu_cpu`, ~700 bytes of
> bank 0, correct at `INT64_MIN` because it applies the sign to unsigned
> magnitudes rather than trusting C89's implementation-defined direction). The
> verb's real value is that a program which *does* multiply — hashing, base32,
> arithmetic-heavy Nix — gets 18x on the operation that would otherwise dominate
> it. The honest summary is that the gate is passed per-operation and is nearly
> invisible per-program, and both numbers belong here.

`DFASTEP` walks the lexer DFA table. A lexer is the one genuinely tight loop in
parsing, so this is what makes L5 self-hosting affordable — the grammar itself stays
on the CPU.

### Deferred (L7)

| Op | Name | Gate |
|---|---|---|
| `$70` | `ED25519_VERIFY` | Only once HTTP fetching is in scope |

## Unproven verbs

These are **not** in the ISA and must pass `checks.verb-economics` before being added:

- `NEXTOP` (bytecode dispatch assist) — plausibly a big win, since interpreter dispatch
  is typically 30–50% of runtime, but 4 port accesses may cancel the benefit.
- Stack push/pop ports — the generational analysis says stacks belong in fast RAM, so
  these are likely dead. Measure before believing otherwise.

## Error codes

`STATUS[4:0]` when `ERR` is set:

| Code | Meaning |
|---|---|
| `$01` | `BADVERB` — unknown opcode |
| `$02` | `NOTFOUND` — `LOOKUP` miss |
| `$03` | `OOM` — allocation failed, collection needed |
| `$04` | `BADADDR` — address outside card memory |
| `$05` | `BUSY` — verb issued while another was running |
| `$06` | `BADPARAM` — malformed parameter slot |
| `$07` | `OVERRUN` — `DATA` written in push mode with the input FIFO full |
