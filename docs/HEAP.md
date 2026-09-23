# Heap object layout (normative)

The card's mature space holds Nix values. This document defines their encoding;
`rtl/heap.v`, `rtl/keycam.v` and `target/heap/` must agree with it.

## Why the memory is *outside* the RTL

`a2nix_card.v` exposes a memory port rather than containing an array. That is not
laziness — it matches the hardware. An ECP5-25F has ~128 KB of block RAM total, which
cannot hold a Nix heap, so the heap lives in off-chip SDRAM on a real board and in a
plain array in the harnesses.

Keeping the port simple (one-cycle, BRAM-like) also means the SDRAM controller and the
4-way tree-PLRU cache from [DECISIONS.md](DECISIONS.md) D12 can be inserted *behind*
it later without touching the card's logic.

### The port is 32 bits wide, and that is load-bearing

`mem_addr` is a **byte** address; the memory returns the **aligned word containing
it**, so the low two bits select a lane rather than shifting the word. Writes carry
`mem_wstrb` byte enables, so a single-byte store needs no read-modify-write.

It was byte-wide first, and the measurement forced the change. `LOOKUP` came out only
**1.5× cheaper** than a 65816 binary search, because filling a CAM batch cost
32 × 4 bytes × 2 cycles ≈ 256 card cycles and the CPU observed that as ~27 `DONE`
polls — the one-cycle comparison was lost in the noise, and the CAM's 1400 LUT4 and
1024 flops were buying almost nothing.

At 32 bits a lane is one access: **43 → 23 bus accesses, 1.5× → 2.7×**. Area went
slightly *down* (9186 → 9099 LUT4), because the byte-assembly registers the engine
needed disappeared entirely.

Two honest notes:

- `HASH_STREAM` barely benefited (55 → 53 accesses). The hasher only accepts a byte
  every two cycles regardless, so it was never memory-bound.
- This is why every column below is 4-byte aligned. That alignment was speculative
  when written; it is now what makes single-access column reads possible.

## Object header

Every object starts with a 4-byte header:

```
+0   tag        1 byte
+1   aux        3 bytes, little-endian   (count, or length, as the tag dictates)
+4   payload    tag-dependent
```

Pointers are 32-bit little-endian byte addresses into the heap. `$00000000` is the
null pointer, so address 0 is never allocated.

## Tags

| Tag | Name | `aux` | Payload |
|---|---|---|---|
| `$00` | `FREE` | — | — |
| `$01` | `INT` | — | 8 bytes, little-endian signed |
| `$02` | `BOOL` | — | 1 byte, `0`/`1` |
| `$03` | `NULL` | — | — |
| `$04` | `STRING` | byte length | bytes, then a 4-byte pointer to the context list |
| `$05` | `PATH` | byte length | bytes |
| `$06` | `LIST` | element count | `count` × 4-byte element pointers |
| `$07` | `ATTRS` | attribute count | three parallel arrays — see below |
| `$08` | `THUNK` | — | 4-byte code pointer, 4-byte env pointer |
| `$09` | `LAMBDA` | — | 4-byte code pointer, 4-byte env pointer |
| `$0A` | `INDIRECT` | — | 4-byte pointer. What `FOLLOW` chases |
| `$0B` | `BLACKHOLE` | — | —. Under evaluation; re-entry is an infinite recursion |
| `$0C` | `FORWARDED` | — | 4-byte pointer to the object's copy in to-space. **Only ever in from-space, only during a collection** |

`STRING` carries a context pointer because Nix tracks which store paths a string value
depends on, and that must propagate at runtime (see ARCHITECTURE.md).

`INDIRECT` exists because forcing a thunk overwrites it in place. When the result is
larger than the thunk, the thunk becomes an `INDIRECT` to the new object rather than
being moved — which is why chasing chains is common enough to deserve a verb.

## ATTRS is struct-of-arrays, deliberately

An attribute set stores its three columns separately rather than interleaving them:

```
+0                      tag = $07, aux = count
+4                      key hashes   : count x 4 bytes   <-- contiguous
+4 + 4*count            value pointers: count x 4 bytes
+4 + 8*count            name pointers : count x 4 bytes
```

The key hashes are contiguous **so the CAM can stream them**. `LOOKUP` compares a
search hash against 32 stored hashes per pass; interleaving the columns would force a
strided read and throw away most of the width. This is the one place in the design
where the hardware dictated the data structure, and it is worth the awkwardness:
attribute lookup is the hottest operation in Nix evaluation.

Attributes are stored **sorted by name**, as Nix requires, so `builtins.attrNames` is
a straight walk of the name column and the CAM's match index is directly usable.

A key hash collision is possible, so a CAM match is a *candidate*: the caller must
still compare the name. `LOOKUP` returns the match index; the 65816 confirms it with
`MEMCMP`. Getting this wrong would silently return the wrong attribute.

**The key hash is djb2**, computed by the CPU: `h = 5381; for each byte c: h = h*33
+ c`, 32-bit wrapping. The card never computes it — `LOOKUP` takes the search hash
as a parameter — so the choice is the 65816's: `h*33` is a shift and an add, ~30
cycles per byte, where FNV-1a's multiply would cost ~200. It is not a cryptographic
hash and is not meant to be; the name compare after the match is what makes the
lookup correct.

**Young attribute sets are searched by the CPU** (a linear scan of the hash column,
then the name compare), because the card cannot read CPU RAM. `LOOKUP` serves sets
that have been promoted. Both paths exist and both are measured; D6 says the
long-lived sets — the ones looked up most — are the promoted ones.

`STRING` bytes are **zero-padded** to the word boundary, so two strings of equal
length compare equal by whole words.

**Bit 7 of `aux2` (bit 23 of `aux`) is a transient visit mark.** The VM's deep
force sets it on a `LIST` or `ATTRS` while descending into it and clears it on the
way out, so a cyclic structure (`let as = { y = as; }`) terminates as Nix's
`deepSeq` does. No real count reaches bit 23, nothing on the card runs while the
CPU deep-forces, and no object is ever handed to the card with the mark set.

## Alignment and minimum size

Headers are 4-byte aligned. `ALLOC` rounds every request up to a multiple of 4, so
pointer columns are always aligned and the 32-bit memory port reads them in one
access.

**Every object is at least 8 bytes** — header plus one word — even `NULL` and
`BLACKHOLE`, whose payload is empty. That word is where the collector writes the
forwarding pointer when it copies the object; a 4-byte object would have its
forwarding pointer land on the next object's header.

`STRING`'s context pointer sits at offset `4 + round4(len)`, i.e. the first aligned
word after the bytes, so it is always a whole word the scanner can read directly.

## The VM and the two spaces (L4 slice 4)

This is D6 meeting the VM. Decided with Chris (2026-09-22): go straight to
promotion rather than an in-RAM semispace first.

**Where values live.** Every allocation the VM makes is in the **nursery** —
bank 1, 64 KB ([VM.md](VM.md)), where a young pointer is `$8001xxxx` and
dereferences directly because the hardware ignores bit 31 — bump-allocated
(`nfree`). Constants materialised from the `.nbc` image sit
at the bottom of the nursery below a watermark and are never moved or scanned (they
hold no pointers). The VM's own static objects (`true`, `false`, `null`, the type
names) are in its binary and are recognised by address. Everything that survives a
collection becomes **mature**: it is copied to the card and its pointer loses bit
31.

**When a collection happens.** Only between two bytecode instructions: `run`
checks `nfree` against a margin before every dispatch. At that point no zero-page
scratch holds a live object (the rule data.s already enforces: anything that runs
bytecode may clobber all of it), so the roots are exactly:

| Root set | Where |
|---|---|
| the value stack | `VSTACK..vsp`, 32-bit pointers |
| the current environment | `env` |
| every frame's objects | the **root stack** `RSTACK`: FORCE and CALL frames, and every primop loop that calls back into Nix, keep their environment/thunk/list/function pointers there instead of as raw 16-bit words on the CPU stack, so the collector can find and update them |
| thunks under evaluation | `FSTACK` |
| catch frames | the `env` in each `CATCH` entry |

An instruction that needs more than the margin in one go fails with `E_NOMEM`
rather than collecting mid-instruction; the margin is sized so that does not
happen on the corpus.

**The collection** is `target/heap/nursery.s`'s algorithm on these roots: a Cheney
copy of the live graph into a staging area in bank 1 (long addressing; the nursery
is read with `(dp),y`, staging written with `[dp],y`), then `RESERVE` on the card
and **one `DATA` burst** of the staging area with every young pointer translated by
the constant `card = base + (staging − STAGE)`. Then every root is rewritten to its
mature address and the nursery is empty again. Survivors cross the bus once, at
about one access per byte; garbage never touches it.

**Reading a mature object.** Bit 31 clear means "on the card". `deref` copies the
object — header and up to 252 bytes of payload — through the card's address
window into one of four **copy windows** (an LRU keyed by card address; four
256-byte arrays in the VM's data), and
returns the window's address, so every accessor written for young objects works
unchanged on the copy. Fields past the window
(long strings, big lists and sets) are read individually through the address port.
A copy costs `1 + 4 + size` bus accesses; a hit costs nothing, which is what makes
the four windows worth their 1 KB.

**Attribute lookup on a mature set** does not copy the set: it hands the search
hash, the hash column's card address and the count to the **`LOOKUP` verb** (the
32-lane CAM), copies only the candidate's name to confirm it, and reads the value
pointer. This is the operation the CAM was built for, and slice 4 is where it gets
measured on real programs instead of micro-benchmarks. Young sets are still a CPU
hash scan.

**Write barrier** (D6's rule, "a mature object never points into the nursery"): the
two places the VM writes into an existing object are `ENV_SET` (a binding into its
`let`'s environment, which may have been promoted while an earlier initialiser
allocated) and the in-place update after forcing a thunk. When the target is
mature, the value being stored is promoted first — a collection with a single root
— and the store goes to the card through the address port.

**A discrepancy this exposes, to resolve before the card's own collector runs:**
the trace table below says `THUNK`/`LAMBDA` word 0 (the code pointer) is a heap
pointer, but the VM stores a bank-0 code address and a constant-table address
there, which the card's `MARK` would chase into the heap. Promotion (this slice)
never asks the card to trace, so it is safe for now; card-side collection (slice
5) must either give code objects a heap representation or teach the tracer that
word 0 of a closure is opaque. Recorded here so it cannot be forgotten.

## What the collector traces

The card's collector (`MARK`/`COMPACT`, see [CARD-ISA.md](CARD-ISA.md)) is a
**Cheney semispace copier**: the heap is two halves, live objects are copied from one
to the other, and nothing dead is ever touched. That is what makes it O(survivors)
(Reduceron memo 28's property, [DECISIONS.md](DECISIONS.md) D15).

To copy an object it needs two facts per tag — its size, and which words are
pointers:

| Tag | Size | Pointer words |
|---|---|---|
| `INT` | 12 | none |
| `BOOL` | 8 | none |
| `NULL`, `BLACKHOLE` | 8 | none |
| `STRING` | `4 + round4(len) + 4` | the last word (context) |
| `PATH` | `4 + round4(len)` (min 8) | none |
| `LIST` | `4 + 4·count` | all `count` words |
| `ATTRS` | `4 + 12·count` | words `count`…`3count−1` — the value and name columns. **The hash column is not pointers** and must be skipped |
| `THUNK`, `LAMBDA` | 12 | both words |
| `INDIRECT` | 8 | the one word |

`ATTRS` is the one that could go silently wrong: its first column is 32-bit hashes
that *look* like plausible addresses. Tracing them would "evacuate" random memory.
