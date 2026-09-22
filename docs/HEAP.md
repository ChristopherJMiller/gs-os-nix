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

## Alignment

Headers are 4-byte aligned. `ALLOC` rounds every request up to a multiple of 4, so
pointer columns are always aligned and a future wider memory port can read them in one
access.
