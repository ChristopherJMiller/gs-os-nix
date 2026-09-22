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
| `$C0n1` | `CTRL` | W | `b0` RESET, `b1` IRQ_EN, `b2` ABORT |
| `$C0n1` | `ID` | R | `$A2` magic, then version via `PPORT` |
| `$C0n2` | `ASEL` | W | Select address register `A0..A3`; resets its byte index |
| `$C0n3` | `APORT` | RW | Sequential bytes of selected address register (LSB first, 4 bytes); auto-increments byte index |
| `$C0n4` | `DATA` | RW | Heap/store data at selected address register; **auto-increments that register** |
| `$C0n5` | `TAG` | R | Tag byte of the most recent `DATA` read |
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

## Verbs

Written to `CMD`. All set BUSY, then DONE or ERR. Operands are named by the register
they read.

### Hashing and store paths

| Op | Name | In | Out | Layer |
|---|---|---|---|---|
| `$10` | `HASH_INIT` | — | resets streaming hash state | L1 |
| `$15` | `HASH_PUSH` | enters push mode; writes to `DATA` are absorbed | — | L1 |
| `$11` | `HASH_STREAM` | `A0` = start, `LEN` = bytes | absorbs card-resident bytes | L3 |
| `$12` | `HASH_FINAL` | — | `P0` = digest, **`P3` = 32-char nixbase32** | L1 |
| `$13` | `NARHASH` | `A0` = tree root node | `P0` = digest of the NAR serialization | L6 |
| `$14` | `STOREPATH` | `P0` = digest supplied by the caller | `P3` = 32-char nixbase32 | L1 |

`HASH_PUSH` exists because M2 must work before the card has any memory of its own
(that arrives at L3), so the CPU has to hand bytes over directly. It stays useful
after L3 for data the CPU holds in main RAM and has no reason to park on the card.

> ### ⚠️ Measured economics: 13.7 bus accesses per byte
>
> `card_main.cpp` counts the real cost, and the first implementation is **far worse
> than the design claimed**. The estimate was ~1 access/byte for a ~300x win over
> software SHA-256. Measured: **13.7 accesses/byte**, because the CPU must poll READY
> before every single byte and spins ~65 cycles per block while the hasher compresses.
>
> | | accesses/byte | ≈ cycles/byte | vs software |
> |---|---:|---:|---:|
> | software SHA-256 on 65816 | — | ~3125 | 1x |
> | `HASH_PUSH`, poll-per-byte (now) | 13.7 | ~137 | **~23x** |
> | `HASH_PUSH` + 64-byte input FIFO | ~1.1 | ~11 | ~280x |
>
> 23x is still a real win and M2 is achievable on it, but it squanders an order of
> magnitude. **Fix: a 64-byte input FIFO** so the CPU streams a whole block at bus
> rate and polls once per block instead of once per byte. One BRAM, and READY then
> means "the FIFO has space" rather than "the hasher is idle".
>
> This is `checks.verb-economics` doing its job (DECISIONS.md D5). The claim in
> ARCHITECTURE.md of a 100-300x hashing win is only true *with* the FIFO.

`HASH_FINAL` yields the encoded path in `P3` as well as the raw digest in `P0`,
because `storepath.v` is purely combinational — once the digest settles, the 32
characters cost nothing. `$14 STOREPATH` remains for folding a digest the caller
computed some other way.

### Flow control in push mode — not optional

The slot bus has no back-pressure, and the hasher stops accepting bytes for ~65 cycles
while it compresses each full 64-byte block. **The 65816 must poll `STATUS` bit 4
(READY) before every `DATA` write in push mode**, or bytes are silently dropped and
the digest is quietly wrong — the worst possible failure mode for a hash.

The expected loop is therefore:

```
  CMD <- $10 (HASH_INIT)
  CMD <- $15 (HASH_PUSH)
  for each byte:
      repeat: b = STATUS  until b & $10
      DATA <- byte
  CMD <- $12 (HASH_FINAL)
  repeat: b = STATUS  until b & $40      ; DONE
  PSEL <- 3 ; read 32 bytes from PPORT   ; the store path
```

`HASH_INIT` is required before pushing: after a `HASH_FINAL` the hasher parks in its
done state with READY low, so pushes without a re-init would stall forever.

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
| `$21` | `FOLLOW` | `A0` | `A1` = first non-indirection in the chain |
| `$22` | `MEMCMP` | `A0`, `A1`, `LEN` | `P0` = sign of comparison |
| `$23` | `MEMCPY` | `A0` → `A1`, `LEN` | — |
| `$24` | `INSTANTIATE` | `A0` = template, `A1` = dest, `P0` = substitutions | wide structured heap write |
| `$40` | `MARK` | `A0` = root list, `LEN` = count | marks reachable mature objects |
| `$41` | `COMPACT` | — | O(survivors) mark-compact |

`FOLLOW` and `MEMCMP` exist because chains and searches amortize the bus crossing.
A *single* dereference must stay on the CPU.

`INSTANTIATE` is the one surviving idea from Reduceron: a template becomes a single
wide card-side structured write rather than word-at-a-time construction.

### Evaluation support

| Op | Name | In | Out |
|---|---|---|---|
| `$30` | `LOOKUP` | `A0` = attrset, `P0` = key hash | `A1` = value address, or ERR `NOTFOUND` |
| `$50` | `ALU64` | `P0`, `P1` operands, `P2` = op selector | `P3` = 64-bit result |
| `$60` | `DFASTEP` | `A0` = text, `P0` = DFA state | `P1` = token type + length |

`LOOKUP` uses a 32-wide key-hash CAM, collapsing attrset lookup from O(log n) pointer
chases plus string compares to O(1). This is *the* hot loop in Nix evaluation.

`ALU64` exists because the 65816 has **no multiply or divide at all** and Nix integers
are 64-bit.

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
