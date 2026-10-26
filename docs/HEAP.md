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
depends on, and that must propagate at runtime (see ARCHITECTURE.md). The pointer is
`0` or a `LIST` of `STRING`s in Nix's element syntax, sorted and unique
([DERIVATION.md](DERIVATION.md) "String context").

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
still compare the name. `LOOKUP` returns the match index; the 65816 confirms it
against its cached copy of the set (`data.c find_attr`; confirming with `MEMCMP`
was measured dearer, CARD-ISA.md). Getting this wrong would silently return the
wrong attribute.

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
`deepSeq` does. No real count reaches bit 23, and nothing on the card runs while
the CPU deep-forces. A collection during the descent promotes the marked
container with its mark set, and the way out clears it through the write
barrier, so the card can hold a marked object — harmless while nothing
card-side reads `aux2`, and one more reason the card-side tracer of slice 5 must
mask it (see the discrepancy note below).

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
(`nfree`). Constants materialised from the `.nbc` image are roots like any other
(the constant tables), so the first collection promotes them and later ones ignore
them: nothing is pinned in the nursery, which is what lets the invariant below
hold without exception. The VM's own static objects (`true`, `false`, `null`, the
type names) are in its binary, in bank 0, and are neither young nor mature.
Everything that survives a collection becomes **mature**: it is copied to the card
and its pointer loses bit 31.

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

**One other place may collect: `nursery_reserve(bytes)`** (vm.h, 2026-09-24),
called by every operation that allocates in proportion to its input (`genList`,
`map`, `++`, `//`, list and set literals, string `+` and interpolation,
`concatLists`, `attrNames`, …) before it pops its operands, while everything
live is still on a stack. If fewer than `bytes` are free, it collects there,
and only if a collection cannot make room is it `E_NOMEM`. This used to be
the rule "an instruction that needs more than the margin fails", which made a
program's fate depend on where the nursery happened to be: `genList 300`
allocates 13 KB in one instruction, and failed or not by position.
`A2MIN_RESERVE_STRESS=1` makes every reserve collect, needed or not, which is
what tests each call site's claim that nothing live is in a C local. An
operation that keeps a *view* across a reserve that collected must take it
again (`view_two`, `view_top_as`): a young operand has moved, and its old copy
still has the right length but a forwarding pointer in its first word.

Two limits this makes visible. **Without a card**, live data that nearly
fills the 64 KB nursery made the collector storm: each collection copied all
of it and freed only a few KB. `eval-okay-big-operations` ran for billions of
cycles that way (58 KB live, ~6 KB freed a time), and the boundaries had
always done the same; the reserve only let programs get far enough to show
it. So a collection that leaves less than `NURSERY_LOW` (8 KB) free is now
`E_NOMEM`, the overhead-limit rule. With a card it cannot fire, because
promotion empties the nursery every time; it only turns the no-card
configuration's hang-shaped failure into a loud one. **Under GC stress**, every
value is promoted before it can die, and nothing collects mature space yet
(the card-side `MARK` is slice 5), so a stress run fails with `E_NOMEM` once a
program has allocated about 512 KB in total, half the harness card's heap.

**The collection** is `target/heap/nursery.s`'s algorithm on these roots: a Cheney
copy of the live graph into a staging area in bank 1 (long addressing; the nursery
is read with `(dp),y`, staging written with `[dp],y`), then `RESERVE` on the card
and **one `DATA` burst** of the staging area with every young pointer translated by
the constant `card = base + (staging − STAGE)`. Then every root is rewritten to its
mature address and the nursery is empty again. Survivors cross the bus once, at
about one access per byte; garbage never touches it.

**Reading a mature object.** Bit 31 clear means "on the card". `deref` copies the
whole object through the card's address register and `DATA` port into a **window**
and returns the window's address, so every accessor written for young objects
works unchanged on the copy — including the three columns of a large set. The
windows are a 60 KB ring in bank 5 (`target/vm/c/data.c`): copies are laid down
one after another, and a direct-mapped table of the 64 most recent makes a
repeated `deref` of the same object — an environment read at every variable, a
closure body — free. A copy costs `4 + 4 + size` bus accesses (set `A0`, read
the header, read the object). A view is never stored: the VM keeps the pointer
it dereferenced, and stores that.

**How long a view lives, and why it is a bytecode boundary.** The ring is
dropped wholesale and restarted **only at a bytecode boundary** — the same
moment the collector is allowed to run, and for the same reason: it is the one
point at which no view is live. So a view is valid for exactly the instruction
that took it. Within an instruction the ring only bumps forward; if it runs out,
that is `E_NOMEM`.

That rule is not a convenience, it is what makes the design sound. Several
operations hold two views at once and read from both — equality descending
through two attribute sets, `find_attr` comparing a name against a set — and a
ring that wrapped whenever it happened to be full could lay a fresh copy on top
of a view its caller was still reading, and answer wrongly *in silence*. Two
sets of 30 KB each once filled the ring exactly, so the next name copy wrapped
onto the first set's header. Turning the ring over only where no view exists
removes the whole class: the failure that remains is loud and is a missing
byte of ring, not a wrong answer.

The price is two constants (`vm.h`): `WINDOW_MAX` (20 KB) is the largest object
that can be viewed at all, and `WINDOW_FLOOR` (16 KB) is the ring position past
which a boundary turns it over — which leaves every instruction 44 KB, more than
the two `WINDOW_MAX` views plus names that the widest operation holds. A mature
string or list bigger than `WINDOW_MAX` is `E_NOMEM`; nothing in the corpus is,
and the real answer for large sets is to stop copying them whole and read single
columns instead, which is what the struct-of-arrays layout above was for.

**Attribute lookup on a mature set** hands the set's card address and the search
hash to the **`LOOKUP` verb** (the 32-lane CAM), which returns a candidate index;
the VM confirms the candidate's name and, if a hash collision put a different name
there, falls back to the CPU scan of the copy so a collision can never lose an
attribute. This is the operation the CAM was built for. Young sets are a CPU hash
scan.

**Write barrier** (D6's rule as weakened by D20, "a mature object never points into
the nursery except through a recorded field"): the places the VM writes into an
existing object are `ENV_SET` (a binding into its `let`'s environment, which may
have been promoted while an earlier initialiser allocated), the in-place update
after forcing a thunk (and the blackholing before), and the visit mark of a deep
force. When the target is mature, the store goes to the card through the address
register, and if what it writes is a young pointer, the field — (object, offset) —
goes in the **remembered set** (`gc.c`, 256 entries in bank 5 above the window
ring). The test is on what is *written*: a thunk forced to an `INT`, `BOOL` or
`NULL` copies the value in and stores no pointer at all, and a closure copied over
a thunk writes its environment, which can be young even when the closure is not.
The next collection reads each recorded field back, re-checks against the
object's header that the offset is still a pointer field (a thunk can be updated
twice, and an `INT`'s high word can look exactly like a young pointer), evacuates
it as a root, and after promotion writes the card address over it and drops the
field's cached window. A **full** set is the old behaviour: a collection with the
value on the stack as its root, then the store. The primop loops that
call back into Nix (`filter`, `partition`, `concatMap`, `listToAttrs`, `toString`
on a list) would be a fourth place, because the container they fill could be
promoted by a callback mid-loop; they collect their results on the value stack
instead and build the container afterwards, when nothing runs bytecode any more.

**A discrepancy this exposes, to resolve before the card's own collector runs:**
the trace table below says `THUNK`/`LAMBDA` word 0 (the code pointer) is a heap
pointer, but the VM stores a bank-0 code address and a constant-table address
there, which the card's `MARK` would chase into the heap. Promotion (this slice)
never asks the card to trace, so it is safe for now; card-side collection (slice
5) must either give code objects a heap representation or teach the tracer that
word 0 of a closure is opaque. Two more things become the card collector's
problem the day it compacts: the window table caches copies by card address, so
it must be flushed when addresses move (today a `RESERVE` never reuses one), and
an object promoted mid-deep-force carries the `aux2` visit mark, which the
tracer must mask out of the count. And since D20, a mature field may hold a young
pointer until the next nursery collection: `MARK` (and `FOLLOW`) may only be
issued with the remembered set empty, i.e. straight after a nursery collection.
Recorded here so none of it can be forgotten.


### Measured: the barrier makes a promoting workload quadratic

`builtins.foldl' (x: y: x + y) 0 (range 1 n)`, which builds `n` thunks and
forces each, with the phase counters of [VM.md](VM.md):

| `n` | collections | from the barrier | bytes promoted | result |
|---:|---:|---:|---:|---|
| 500 | **0** | 0 | 0 | 125250 |
| 550 | 52 | **51** | 15,972 | 151525 |
| 650 | 101 | **100** | 24,432 | over 60M cycles |
| 1000, **no card** | **3** | **0** | 0 | 500500 |

Read the last row against the others. With no card nothing is ever promoted, so
nothing is ever mature, so the barrier never fires: three collections, all from
memory pressure, and the program finishes. With a card it does not finish. **The
card makes this workload quadratic precisely because it works.**

The mechanism is the rule above, taken literally. Once a collection promotes the
list and its unforced thunks, forcing one of those thunks writes its value into a
*mature* object — and the only way to promote the value is a **whole collection**,
which is O(live). One per ten forces at `n` = 550, and `live` grows with `n`, so
the total is O(n²). Nothing about the bus is involved: the crossing is ~1.5% of
the cycles, and `MEMCPY` or any other verb would not touch it.

This is the cost of "there is no remembered set". A remembered set would make
the store O(1) — write the young pointer through the bus, record the field, and
fix it at the next collection, which then promotes for memory-pressure reasons
only. The invariant would weaken to *a mature object never points into the
nursery except through a recorded field, which the card must not trace*, and
that is safe only while the card's collector does not run. It has to be resolved
before `MARK` does, like the `THUNK` word-0 discrepancy below.

**Built (D20), and re-measured** with the same counters:

| `n` | collections | from a full set | fields remembered | cycles | result |
|---:|---:|---:|---:|---:|---|
| 550 | **1** (was 52) | 0 | 0 | 20.2M (was 38.6M) | 151525 |
| 650 | **1** (was 101) | 0 | 0 | 32.5M (did not finish in 60M) | 211575 |
| 1000 | **1** | 0 | 0 | 85.7M | 500500 |
| 1000, no card | 3 | 0 | 0 | 38.8M | 500500 |

Collections collapsed to memory pressure. But **the set recorded nothing** on
this workload, which says the account above was half wrong: forcing these
thunks yields `INT`s, which are copied into the thunk and put no pointer on the
card. The old barrier promoted the value whenever the value was young, whether
or not a pointer was being stored — so most of the quadratic was a barrier firing
on stores that needed nothing. Testing what is written fixed that; the set
covers what remains (lists, strings and closures), which
`tests/fixtures/eval-okay-remembered-set` exercises.

What is left of the gap to no card (85.7M against 38.8M) is not collections: one
collection, then 4,950 window copies moving 238 KB — every force of a promoted
element thunk reads it through the bus, and the profile's top page ($A500, 25%)
is to be identified before anything is changed (D19).

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
