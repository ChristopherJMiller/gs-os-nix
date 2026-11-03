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
+1   aux        2 bytes, little-endian   (count, or length, as the tag dictates)
+3   marks      1 byte: bit 7 visited (a deep force is in it), bit 6 a `with` environment
+4   payload    tag-dependent
```

The marks are the CPU's, and are never part of the count: the card's
`obj_size`, `is_ptr_field` and `LOOKUP` read 16 bits, as the CPU's `AUX16`
does. (The card read 23 until 2026-10-27, masking only the visit mark: a
`with` environment, a 2-element `LIST` with bit 6 set, was four million
elements long to it -- harmless until `MARK` ran.)

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
| `$09` | `LAMBDA` | 0, or 1 + the chunk that builds its `builtins.functionArgs` set (`LAMBDA_ARGS`) | 4-byte code pointer, 4-byte env pointer |
| `$0A` | `INDIRECT` | — | 4-byte pointer. What `FOLLOW` chases |
| `$0B` | `BLACKHOLE` | — | —. Under evaluation; re-entry is an infinite recursion |
| `$0C` | `FORWARDED` | — | 4-byte pointer to the object's copy in to-space. **Only ever in from-space, only during a collection** |
| `$0D` | `FLOAT` | — | 8 bytes, IEEE-754 binary64 bits, little-endian ([FLOAT.md](FLOAT.md)); 12 bytes in all, no pointers |

`STRING` carries a context pointer because Nix tracks which store paths a string value
depends on, and that must propagate at runtime (see ARCHITECTURE.md). The pointer is
`0` or a `LIST` of `STRING`s in Nix's element syntax, sorted and unique
([DERIVATION.md](DERIVATION.md) "String context").

`INDIRECT` exists because forcing a thunk overwrites it in place. When the result is
larger than the thunk, the thunk becomes an `INDIRECT` to the new object rather than
being moved — which is why chasing chains is common enough to deserve a verb.

## ATTRS is struct-of-arrays, deliberately

An attribute set stores its columns separately rather than interleaving them:

```
+0                      tag = $07, aux = count
+4                      key hashes   : count x 4 bytes   <-- contiguous
+4 + 4*count            value pointers: count x 4 bytes
+4 + 8*count            name pointers : count x 4 bytes
+4 + 12*count           positions    : count x 2 bytes, padded to a word
```

So a set is `4 + 12n + round4(2n)` bytes (8 if empty), and a reservation
counts 16 bytes an attribute (`ATTR_RESERVE`), which covers the padding.

**Positions** are `builtins.unsafeGetAttrPos`'s: an attribute's index into
the image's position table (BYTECODE.md "File layout"), 0 for none. They are
not pointers, so the collector and the card never trace them, and `LOOKUP`
never reads them; they ride along as Nix's attributes carry their `PosIdx`.
A literal set's attributes are the generator's positions -- a binding's is
where its attribute path starts (so in `a.b.c = 1` all three report `a`), an
`inherit`'s the name's, a computed name's its `${`. `//`, `removeAttrs` and
`intersectAttrs` copy them; `listToAttrs` takes the `value` attribute's of
each element; `functionArgs` has the formals'; everything else Nix builds
with `noPos` -- `mapAttrs`, `zipAttrsWith`, `groupBy`, `tryEval`,
`parseDrvName`, `fromJSON`, `fromTOML`, `builtins` itself -- and so do the
VM's (all measured on the host, 2026-10-26). They cost 2 bytes an attribute,
about 15% of a set.

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
`deepSeq` does. No real count reaches bit 23, so **every card-side reader of
`aux` masks it out**: `LOOKUP`'s attribute count, and `obj_size` and
`is_ptr_field` for the tracer. A collection during the descent promotes the
marked container with its mark set, and the way out clears it through the
write barrier, so the card holds marked objects -- and the card does run while
the CPU deep-forces: each thunk the descent forces runs bytecode, and a
`SELECT` on a promoted set is `LOOKUP`. This text once said nothing on the card
ran then, and `LOOKUP` took all 24 bits as the count. Found 2026-10-27, by the
corpus under GC stress: `let a = { b = 1; c = a.b; }; in a` read `b`'s value
from `4 * 2^23` bytes past the set -- null, so `E_BADNBC`, but where that
memory held a pointer it would have been a silent wrong answer
(`eval-okay-deep-force-self-select`, regress "LOOKUP finds every key in a
set the CPU has marked", and an RTL mutation guard it).

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
value is promoted before it can die; until mature space was collected
(2026-10-27, below) a stress run failed with `E_NOMEM` once a program had
allocated about 512 KB in total, half the harness card's heap (1 MB then).

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

**A discrepancy this exposes, to resolve before the card's own collector runs**
(resolved 2026-10-27: "Collecting mature space", below):
the trace table below says `THUNK`/`LAMBDA` word 0 (the code pointer) is a heap
pointer, but the VM stores a bank-0 code address and a constant-table address
there, which the card's `MARK` would chase into the heap. Promotion (this slice)
never asks the card to trace, so it is safe for now; card-side collection (slice
5) must either give code objects a heap representation or teach the tracer that
word 0 of a closure is opaque. Two more things become the card collector's
problem the day it compacts: the window table caches copies by card address, so
it must be flushed when addresses move (today a `RESERVE` never reuses one), and
an object promoted mid-deep-force carries the `aux2` visit mark, which the
tracer masks out of the count (`obj_size`, `is_ptr_field`; as `LOOKUP` does). And since D20, a mature field may hold a young
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
| `INT`, `FLOAT` | 12 | none |
| `BOOL`, `NULL` | 8 | none |
| `STRING` | `4 + round4(len) + 4` | the last word (context) |
| `PATH` | `4 + round4(len)` (min 8) | none |
| `LIST` | `4 + 4·count` (min 8) | all `count` words |
| `ATTRS` | `4 + 12·count + round4(2·count)` (min 8) | words `count`…`3count−1` — the value and name columns. **The hash column is not pointers** and must be skipped, and nor are the 2-byte positions after the names (D28) |
| `THUNK`, `LAMBDA`, `BLACKHOLE` | 12 | word 1, the environment. **Word 0 is not a pointer**: the VM's code and constant-table offsets (`CL_CODE`, `CL_CTAB`) |
| `INDIRECT` | 8 | the one word |

Every count is the header's 16 bits, never its marks byte (the visit and
`with` marks, "Object header" above), on both sides. Every row
is the CPU collector's (`gc.c` `obj_size`, `scan_one`) and the card's
(`rtl/a2nix_card.v` `obj_size`, `is_ptr_field`) alike, and must stay so: a
disagreement copies the wrong bytes or follows a word that is not a pointer,
in silence. Mature stress (below) runs every fixture through both.

`ATTRS` is the one that could go silently wrong: its first column is 32-bit hashes
that *look* like plausible addresses. Tracing them would "evacuate" random memory.
A closure's word 0 is the other: `(ctab << 16) | code`, which for a constant
index under 8 falls inside half A (and under 16, half B). The card traced it
until 2026-10-27, when the VM first called `MARK` -- `regress` now has a canary
for it -- and it sized a `BLACKHOLE` by the 8-byte default. A `BLACKHOLE` is a
`THUNK` under evaluation, and the update that ends the force writes 12 bytes
into it: copied as 8, over the next object's header. Both are RTL mutations.

## Collecting mature space

Until 2026-10-27 nothing did: the card had `MARK` and `COMPACT` (L3) and the VM
never called them, so a program could promote 512 KB in all -- half the
harness card's heap, 1 MB then -- and a GC-stress run of `eval-okay-tail-call-1` reached
that. Now a collection of mature space follows a nursery collection, and the
four things "The VM and the two spaces" listed to resolve first are resolved:
a closure's word 0 is opaque and a `BLACKHOLE` 12 bytes (the table above);
the window table is flushed, since every copy is of an old address; the visit
mark is masked; and `MARK` only runs with the remembered set empty, because it
only ever follows a promotion, which empties it.

**When.** After every promotion, if the card's current half has less than
`MATURE_MARGIN` free -- the most the next promotion can need, a whole nursery
(64 KB), and the largest root list (below, 9.5 KB): ~73 KB. The CPU knows the
card's allocation pointer exactly without asking: every `RESERVE` returns its
base and moves it by `max(4, round4(n))`, and a promotion's size is a
multiple of 4, at least 8; the half's end is `card_half` after the half's
start, the size the card reports (`GEOMETRY`, which `card_find` asks; the
core's input `heap_half_log2`): 8 MB on a2min, MAME and rev A, 512 KB in the
tests that must fill a half. Checked after every promotion, the margin is
there before the next, so its `RESERVE` cannot fail. If a collection still
leaves less than the margin, live data has filled the half, and it is
`E_NOMEM` -- the overhead limit, as the nursery's -- rather than a
collection at every promotion. So the card holds its half less ~73 KB of
live data (~8.1 MB; ~439 KB on 512 KB halves), and a program may promote
without limit.

**How** (`gc.c` `gc_mature`). The roots are the nursery collector's, in its
order: the value stack, `env`, the root stack, the force stack, each catch
frame's `env`, the constant table, the import memo, `drv_table`. They go to a
block `RESERVE`d in from-space, which the flip abandons, straight from the
arrays they live in, one `DATA` burst an array; `MARK` copies what they reach
into to-space and writes each one's new address over it; the block is read
back into the same arrays; `COMPACT` flips; a `RESERVE` of 4 says where
allocation resumes; the window ring is turned over. A young pointer, a static
one (bit 31 set) or null passes through `MARK` unchanged, so the CPU sorts
nothing out. The bus carries about 8 accesses a root; the copying is the
card's, O(survivors).

**Where it may run.** Wherever a nursery collection may: a bytecode boundary,
`nursery_reserve`, and the two places a full remembered set collects,
`store_field` and the update of a mature thunk. Those two held their mature
object in a C local across the collection, which was safe only while nothing
moved mature objects (`gcsafe: mature`, the lint's two exceptions); they keep
it on the root stack now and read it back, and the exceptions are gone. A
view is dead after any collection already (vm.h's rule, which the lint
enforces), so turning the ring over mid-instruction loses nothing.

**Tested by** `A2MIN_MATURE_STRESS=k`, under which every *k*-th promotion is
followed by a mature collection, needed or not: with GC stress, every fixture
runs through the moving collector, and with reserve stress every reserve
does. It found the third disagreement in the table above: five fixtures
that use `with` were `E_NOMEM`. `eval-okay-mature-collection` promotes 1.45
MB with little live -- three mature collections in a plain run on 512 KB
halves, both flips, where it was `E_NOMEM` at 530 KB. On 8 MB halves it fills
none, so `checks.scoreboard` also runs it on 512 KB ones and requires the
three (`scripts/small-card.sh`, `just fixtures-small-card`). A root left out of `MARK`'s list does not fail at once:
its old address keeps the old bytes until a later collection copies into
that half, so a test has to move an object twice and then use it.
`eval-okay-tryeval-mature` does that for the one root nothing else reached,
a catch frame's environment: under mature stress, 1,248 collections inside a
`tryEval`, then a throw, then the `let`'s variables read through the frame.

## Long lists (design, 2026-10-29)

**Why.** Real Nix programs build and walk lists of thousands and tens of
thousands of elements; the VM's list primops give out far sooner, with
`E_NOMEM`. M3's first session met it at once (COMPILER.md "Stage 4", "What
the session found"). Measured on `a2min --run-nix` (2026-10-29), each primop
fed a list built by `concatLists`:

| Primop | Works to about | What ends it |
|---|---|---|
| `sort`, `partition` | 493 elements | the value stack (1,024 slots): two slots an element |
| `filter`, `concatMap` | 971 | the value stack: the kept elements, or the mapped lists, staged on it |
| `genList` | 1,460 | the reservation up front: 44 bytes an element (`make_apply`'s 28, the index's INT 12, the slot 4) |
| `map` | 1,995 | the reservation up front: 32 bytes an element |
| `concatLists`, `length`, `elemAt` | 10,193 | the nursery: the result is allocated whole, with its parts live |

Nix has no such ceilings. Chris's decisions (2026-10-29): lists come first
of the three items after M3, in **two steps** -- every list primop to the
nursery's size, then lists past the nursery, as big as the card holds;
**the card's heap grows** for it, since 1 MB cannot hold the lists either
step asks for; and step 1 covers **every list primop**, big attribute sets
and long strings becoming a later item of their own.

**Goals.** Step 0: a card heap big enough for the targets, the size the card
reports rather than a constant. Step 1: every primop that builds or reads a
list works up to the largest list the nursery can hold (~16,000 elements:
the list object, 4 bytes an element, has to fit in it). Step 2: lists up to
65,535 elements (below), allocated on the card when they cannot be young,
read an element at a time. At each, the answers are the host's, under every
stress mode.

**Non-goals.** A new list layout -- chunked lists, ropes -- since every
consumer, the card's tracer and the printer would change with it; counts
past 65,535; attribute sets past their own ceilings (one over ~4,600
attributes cannot be young, one over ~1,460 cannot be viewed once mature)
and strings past 64 KB (`toJSON` of a long list will meet it) -- the later
item; making long lists fast.

**Anti-goals.** A wrong answer in silence: anything still too big stays a
loud `E_NOMEM`. Small lists slower: the benchmarks are the gate (D29), and a
list that fits as today takes today's path. A list the collector can see
before every slot of it is valid.

**Constraints the design keeps** (each from the code, 2026-10-29):

- **Counts are 16 bits** everywhere -- `AUX16`, the card's tracer
  ("What the collector traces"), the printer -- which caps a list at 65,535
  elements (256 KB). Past it means 24-bit counts in the RTL and the C at
  once: out of scope.
- **`obj_size` is 16 bits**: a list past 16,383 elements overflows it, and
  every size computed from a header with it. Step 2 widens every size
  computation that can see such a list -- `obj_size`, `mature_view`'s check,
  the stage's copy -- before such a list can exist; overflowing silently
  copies the wrong number of bytes.
- **A mature object over `WINDOW_MAX` (20 KB, 5,000 list elements) cannot
  be viewed**: `deref` of it is `E_NOMEM`. A list that is promoted past that
  size can be read only an element at a time (`list_elem`, `RELEM`, or
  `card_read_at` in bursts). Step 1's lists cross it (16,000 elements is 64
  KB), so step 1 already has to fix the readers its own primops use.
- **Forcing and type checks read the tag through a view too** -- `force_top`
  and `FORCE_TOP`, `force_slot`, `view_two`, `force_two_orig`,
  `force_pop_as`, `typeOf` and the `is*` tests, `values_equal`,
  `call_enter`, and the thunk update in `force_thunk` -- so `let l = genList
  (x: x) 15000; in length l` would fail when `l`'s thunk is updated, before
  `length` runs. *My call:* a whole view of a list too long to view stays
  `E_NOMEM`, loud, and those paths read the header alone (its tag and
  count: from the window cache, or 4 bytes across the bus) where the object
  is mature and too long to view. That is the mature path only -- the young
  path, which the dispatch loop's macros take inline, is untouched -- and
  any reader not yet fixed stays loud rather than reading the ring. The
  benchmarks measure what it costs.
- **A list may be seen by a collector mid-fill.** Both collectors pass a
  null pointer through (`gc.c` `evac`; `rtl/a2nix_card.v`, EVAC's null
  test), so a list whose unfilled slots are null is safe to collect; one
  with garbage in a slot is not. A collection promotes everything young, so
  a list being filled may be mature when the next element is stored: stores
  then go through `store_field` (the barrier), and a young value fills the
  remembered set (`RSET_N`, 256 entries), whose filling collects again.
  The card's copier is a semispace copy by the header's count, so a list
  may not grow its count after allocation: the copy would drop what was
  reserved past it.
- **The value stack is 1,024 slots**, the root stack 320: neither may hold
  a list's elements.
- **`card_reserve` takes 16 bits**, though the card's `RESERVE` takes a
  32-bit `LEN` (CARD-ISA.md): a 256 KB list is a C change, not an RTL one.
  An allocation on the card outside promotion has to keep `card_next` and
  `card_end` right, and make room first (a mature collection) as promotion
  does (`MATURE_MARGIN`). Card memory comes back from `RESERVE` with old
  bytes in it.
- The VM's usual rules (the vm-change skill): no `obj_t *` C local across
  anything that collects; bank 0 is nearly full; no 32-bit products
  (`~UMUL4`).

**Step 0: the card's heap, grown** (done 2026-11-03, below). The harness
card's heap was 1 MB, two halves of 512 KB (`SPACE_SIZE` in
`rtl/a2nix_card.v`, `cardbus.h`, `CARD_HALF` in `vm.h`, MAME's
`HEAP_BYTES`), and after `MATURE_MARGIN` about 439 KB could be live. That holds neither step: `genList` keeps ~44 bytes an
element live, so ~10,000 elements fit, where step 1 asks 15,000; step 2's
60,000-element `genList` keeps ~2.6 MB live, and `sort`, `filter`,
`partition` and `tail` hold two or three lists at once. The board has 32 MB
and its memory map keeps the first 16 MB for the heap (HARDWARE.md HW-12,
which leaves the heap's size to this side).

- *My call:* the full 16 MB the board keeps, two halves of 8 MB, in the
  RTL, the harness and MAME together -- unless the measurement of what a
  mature collection of that much costs says less.
- The card reports its heap's size (a register, CARD-ISA.md), and the VM
  reads it at start instead of `CARD_HALF`: the open item "the card's
  geometry is a constant" (STATUS.md), closed here because a second size
  would make the constant wrong somewhere.
- HW-12 and rev A's memory map are the hardware session's: the change is
  agreed with it and its document updated in the same commit.
- `regress`, the RTL mutations and every mature-space gate run at the new
  size; a mature pointer stays a card address with bit 31 clear, which 16
  MB leaves room for.

*Done* (2026-11-03), as called: the core's half is an input,
`heap_half_log2` (`regress` builds cards of 512 KB and 8 MB), reported by
`GEOMETRY` ($26, CARD-ISA.md), which `card_find` asks once and keeps in
`card_half`; a2min and MAME run 8 MB halves (`A2MIN_CARD_HALF` sets a2min's).
The measurement the call waited on: `fixtures-mature` -- a mature collection
after every promotion -- over every fixture but the new one costs
5,521,663,225 CPU cycles at 8 MB a half and 5,521,663,225 at 512 KB,
identical, because `MARK` and `COMPACT` copy what is live, not the half. So
8 MB stands. `eval-okay-card-heap-big` keeps ~1.2 MB live: at 8 MB it
promotes 1,233,004 bytes in 20 collections and answers in 272,845,435
cycles; at 512 KB it is `E_NOMEM` after 492,604 bytes and one mature
collection. Under mature stress it answers at 8 MB (874,786,847 cycles,
5,895 mature collections), but past the scoreboard's 600 s, and that pass
skips it (`scoreboard.py --skip`). The tests that must fill a half --
`eval-okay-mature-collection` (above) and `mutation-vm` -- run on 512 KB
halves, the size they were written against. It costs ~930 CPU cycles and 7
bus accesses a run (the bench, at `card_find`) and 4 bytes of bank 0
(16,403). HW-12 is rewritten to match; rev A's SDRAM map is unchanged.

**Step 1: every list primop to the nursery's size.**

- *Builders in batches.* A primop that makes new objects per element
  (`genList`, `map`) allocates its result list first, nulled, roots it, and
  makes the elements a batch at a time, reserving per batch, so that a
  collection may fall between batches; each store into the list after one
  goes through `store_field`. Below one batch, it reserves once as today.
- *No staging on the value stack.* `filter`, `concatMap`, `sort` and
  `partition` stage their results on the stack today, and so do
  `listToAttrs`, `groupBy` and `genericClosure` (two slots an element),
  `catAttrs`, `zipAttrsWith`, `concatStringsSep` where an element has
  context or must be coerced, and `toString` of a list. Each builds into a
  list instead -- `filter`'s and `partition`'s at the input's length, then
  copied to the exact length (a count may not shrink in place either, for
  the same reason it may not grow); `sort`'s working buffer a second list.
  Where the result is a set or a string, step 1 takes it as far as that
  set's or string's own ceiling, which is the later item's.
- *Their readers element by element* where the list can be past
  `WINDOW_MAX`: `elemAt`, `head`, `tail`, `length` and the rest that call
  `force_pop_as(T_LIST)` and index `ELEMS` read one element (`list_elem`)
  or the header, not a view of the whole. A read an element at a time
  takes no view, or turns the ring over between elements as
  `deep_force_top` does.
- `++` (`OP_CONCAT`, `run.c`) is a builder too, and in step 1.
- The 25 `list_alloc` call sites are the builders' list (`prims.c` 12,
  `run.c` 3, `derivation.c` 5, `json.c` 2, `match.c` 3); most build a few
  elements and stay as they are.

**Step 2: lists past the nursery (to 65,535 elements).**

- A list whose object does not fit the nursery is `RESERVE`d on the card
  directly, its header and nulls written there, and filled as step 1 fills
  a promoted one.
- Every size computation and offset widened first: `obj_size` and its
  callers, `store_field`'s offset and the remembered set's (`rs_entry.off`,
  16 bits), `list_elem`'s, the printer's `rd`. Past 16,383 elements a slot's
  offset does not fit 16 bits.
- Every reader of a list's elements audited -- the ~67 places that index
  `ELEMS` of a view, among them equality and comparison (`data.c`),
  `deepSeq`, `toJSON`, the derivation's arguments -- to read element by
  element when the list is mature and long.

**Gates.** Fixtures with the host's answers, each primop past each old
ceiling. *My call on their modes:* full size in plain runs (and with reserve
stress, which costs little); smaller variants, still past the old ceilings,
in the GC-stress, period and mature-stress modes, sized to the cycle budget
by measurement -- under mature stress every boundary copies all live mature
data, so a 15,000-element list there would take hours; and excused with no
card, as `eval-okay-big-operations` is, since the 64 KB nursery alone holds
none of them. `sort`'s input in a random order, so that its buffer is
used. A mutation per new rule (a slot not nulled before a collection, a
store into a promoted list without the barrier, a long list viewed whole,
an `obj_size` or an offset left 16-bit, a header read where a view is
needed); the benchmarks the same to the noise, small lists taking today's
path.

**Left to the builder:** the batch size; when a builder takes the batched
path; how `sort` lays out its buffer; whether nulling a card list is the
CPU's bus writes or a card verb (an RTL change, and so a D5 measurement);
the order the readers are fixed in.
