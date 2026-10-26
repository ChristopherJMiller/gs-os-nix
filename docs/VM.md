# The VM in C (D18)

The bytecode VM, ported from `target/vm/*.s` to C compiled by ORCA/C
([TOOLCHAIN.md](TOOLCHAIN.md)). [BYTECODE.md](BYTECODE.md) says what it executes,
[HEAP.md](HEAP.md) how values are laid out; this page says how the C program is
shaped, where it lives, and how the port was verified. The assembly VM it replaced
was removed at parity: commit 05ce616 is the last with both, and `promote.s` and
`card.s` — kept as the reference while slice 4 was ported — went with it.

## Memory map

The 65816 has 16 MB; the IIgs under GS/OS gives a program a few banks of it.
The VM is compiled with ORCA/C's **small memory model** (the default): code and
global scalars share one static segment, addressed absolutely through the data
bank register, which `omf2bin`'s stub leaves at bank 0. Everything else is placed
by bank and reached through 32-bit pointers, which ORCA/C dereferences with
`[dp],Y` regardless of model.

```
bank 0
  $0300  RESULT / status / error         the harness contract (BYTECODE.md)
  $0310  DIAG                            the VM's state on error: pc, chunk, vsp, rsp, nfree, depth
  $0330  GC_STRESS / VERB_MASK           a2min sets them: collect at every boundary;
                                         refuse a card verb and do it on the CPU
  $0400  C stack, growing down from $1FFF   7 KB: frames and direct-page locals
  $2000  the program                     code + globals, from omf2bin; ≤ $BFFF
         globals: value stack (4 KB), RSTACK (768 B), FSTACK (512 B), CATCH (12 × 16 B),
                  CTAB (2 KB), CHUNK_INFO (128 × 4 B), the window table (64 sets × 2 ways × 8 B)
  $C000  never used: slot I/O, the card at $C0nX, then language-card RAM/ROM

bank 1  NURSERY   $010000-$01FFFF   64 KB, bump-allocated
bank 2  STAGE     $020000-$02FFFF   the collector's to-space
bank 3  the .nbc image               $030000, up to 64 KB (a2min --run-nbc puts it there)
bank 4+ further code segments        ORCA/C `segment "prims";` etc., placed by omf2bin,
                                     which skips bank 5 when bank 4 fills (bank 6 is
                                     next); the interpreter core stays in bank 0
bank 5  WINDOWS   $050000-$05EFFF   the 60 KB copy ring: views of mature objects
        RSET      $05F000-$05F9FF   the remembered set: 256 × (object, offset, scratch)
```

`$0332` is a third harness flag: `A2MIN_RSET_LIMIT` shrinks the remembered set
so the tests can make it fill. And the GC-stress byte at `$0330` is a period,
not a switch: `A2MIN_GC_PERIOD=k` collects at every *k*-th boundary. At `k` = 1
every value is promoted before it can be stored young, so the remembered set
is never used; `k` = 2 or 3 is what tests it (DECISIONS.md D20).

Bank 0's 40 KB were never going to hold the parser and the store layers, so
code lives in load segments from the start: the primop files are `segment "prims"` and
sit in bank 4; the core (`main`, `run`, `data`, `gc`, `card`) and every global
stay in bank 0. `omf2bin` refuses a bank-0 segment past `$C000` rather than
letting code land on the I/O page — the first time it did, the program ran as
far as `$C0CF` and read the card's registers as instructions.

Two consequences, both recorded in HEAP.md:

- **A young pointer is `$8001xxxx`.** Bit 31 still means "in the nursery"; byte 2
  is the bank. The hardware ignores bit 31 when dereferencing (`[dp],Y` reads three
  bytes), so a young pointer is a valid C pointer as it is; a mature pointer (bit
  31 clear) is a card address and must go through a copy window. Objects never
  straddle the bank end (the allocator refuses), so 32-bit pointer arithmetic
  inside an object never leaves bank 1.
- **The nursery is four times larger** than the assembly VM's 16 KB — the two
  corpus tests that exhausted it (`eval-okay-tail-call-1`, the long-string one) get
  another chance without any change to the collector.

Three ceilings a program can hit, all of them a loud `E_NOMEM` rather than a
wrong answer: `CTAB_N` = **512 constants** across all chunks (a 256-attribute
literal set needs 512 — 256 names and 256 integers — and so fails at load,
which is what stopped the `LOOKUP` crossover measurement at 128); `WINDOW_MAX` =
**20 KB** for a single mature object that has to be viewed; and the nursery's
64 KB of live young data. All three are a constant in `vm.h`, and bank 0 has
room to raise the first. (The nursery ceiling used to be a fourth, worse one:
a single operation that allocated more than happened to be free failed even
when a collection would have made room. `nursery_reserve` removed it; HEAP.md.)
A fifth is the card's: nothing collects mature space yet, so a program can
promote at most half the card's heap in total (512 KB in the harness), which
only GC-stress runs reach. A derivation adds its own: its final ATerm, and
everything else its one reserve covers, must fit the nursery (DERIVATION.md
"Ceilings").

The C stack is the 65816 stack: ORCA/C allocates a function's locals below the
stack pointer and points the direct-page register at them (`tsc / sbc / tcs / phd
/ tcd`), so `FORCE`'s recursion through `run()` costs one C frame per nesting
level, as the assembly's did through `jsr run`. `E_NOMEM` when it nears `$0400`.

## Representation

`vm.h` declares the heap objects of HEAP.md as C structs with explicit
little-endian fields — `uint8_t tag; uint8_t aux[3];` then the payload — and
accessors that keep the tag/aux packing in one place. A value is `obj_t *`
(4 bytes). Nix integers are `long long` (ORCA/C: 8 bytes, the compiler's own
`~MUL8`/`~CDIV8`/`~SCMP8` helpers are pure and are what the assembly VM's word
loops become); `MUL`, `DIV` and `MOD` still go to the card's `ALU64` when a card
is present, because the D5 measurement (CARD-ISA.md) stands regardless of the
language the loop is written in.

The VM's registers are C globals (`pc`, `ctab`, `env`, `vsp`, `rsp`, `nfree`), so
the collector's root set is a fixed list of arrays plus those globals — no
direct-page scanning, no "which zp scratch is live" rule. The rule that cost the
most bugs in the assembly (anything that runs bytecode clobbers every scratch
register, so keep pointers on RSTACK across a force) becomes a type rule: a C
local of type `obj_t *` **must not be held across a call that can collect**;
`rpush()`/`rpop()` are the only way to keep one, and the collector rewrites them.

## Mature objects (slice 4)

A promoted object lives on the card and is reached in three ways (HEAP.md "The VM
and the two spaces" has the rules; this is where they land in the C):

- **Reading:** `deref()` copies it into the window ring in bank 5 and returns the
  copy — a *view*. Views are for reading; the code keeps the pointer it
  dereferenced for anything it stores (`force_pop_view` hands out both). A view
  lasts exactly one bytecode instruction: the ring is turned over at boundaries
  and nowhere else, so a loop that calls back into Nix re-derefs every time
  round (`prims.h` `RELEM`). See HEAP.md “How long a view lives” for why the
  alternative — wrapping whenever the ring filled — was a silent wrong answer.
- **Writing:** `store_field()` / `store_byte()` are the write barrier: a store
  into a mature object goes through the bus and, if it wrote a young pointer,
  records the field in the remembered set (`gc.c remember`, D20); the window
  copy is dropped. Forcing a mature thunk blackholes it on the card and writes
  the update there, recording the one pointer the update can carry. Only a full
  set costs a collection (with the value on the stack as its root).
- **Searching:** `find_attr()` on a mature set is the card's `LOOKUP` verb, the
  name compare, and a CPU scan of the copy if a hash collision misled the CAM.

The collector promotes at every collection: the staging area's pointer fields are
translated in place by `card = base + (offset − start)`, the whole area crosses
the bus as one `DATA` burst into a `RESERVE`d block, and the roots — the value
stack, `env`, the root stack, the force stack, the catch frames' environments and
the constant tables — are rewritten to card addresses. Without a card the
survivors are moved back into the nursery instead.

## What stays assembly

`asm { }` blocks, in the C source, for exactly three things:

1. **The card's ports** — 8-bit stores to `$C0nX` with `sep #$20`; a 16-bit C
   store would write the next register too.
2. **`memcpy`-shaped loops the collector runs** — planned as `mvn`; not done:
   `gc.c` has no assembly and copies with ORCA/C's `memcpy`. Whether `mvn`
   would pay is unmeasured.
3. **The dispatch loop's opcode fetch**, if measurement says the C version is
   slower than a `jmp (table,x)`; until measured, it is a `switch`.

Everything else is C. The port is a translation, not a redesign: each `op_*`
routine in the assembly becomes a `case`, each helper a function with the same
name, so the two can be read side by side while both exist.

## Layout of the source

```
target/vm/c/
  vm.h        object structs, tags, error codes, the register globals, RSTACK API
  opcodes.h   OP_* / PRIM_* — the same table as the generator's opcodes.rs
              (checks.nbc-opcodes diffs both against BYTECODE.md)
  main.c      entry (first in the link: omf2bin takes the first code segment's
              entry), the loader, result reporting, DIAG
  run.c       run(): dispatch, environments, calls, thunks, FORCE, catch frames
  data.c      strings, lists, attribute sets, hashing, equality, deep force,
              the write barrier and the window ring for mature objects
  prims.h     what the three primop files share (private to them)
  prims.c     the primops' shared helpers, lists, sets, errors, the table -- bank 4
  strings.c   string primops, coercion, interpolation, string context -- bank 4
  derivation.c  derivationStrict, the ATerm, fixed outputs, and the
              derivation rule of equality (DERIVATION.md) -- bank 4
  gc.c        the Cheney collector into bank 2, then promotion to the card
              (copy-back into the nursery when no card is present)
  card.c      the driver: ALU64, RESERVE, DATA, LOOKUP, the SHA-256 verbs
  sha256.c    SHA-256 and the store-path fold on the CPU, for no card -- bank 4
```

`target/vm/c/SOURCES` is the one list of sources, in link order: the flake's
`mkOrcaBinary`, `just cvm` and every script that builds the VM read it. Each
object file costs one byte of bank 0 (its `~GLOBALS` segment, measured when
`prims.c` was split), whatever its segment.

## Phase counters and the profiler

Two instruments, both added after four hypotheses about a superlinear cost were
each disproved by a slow experiment (DECISIONS.md D19):

- **Phase counters** (`vm.h STATS_ADDR`, printed by a2min as `phases:`):
  collections, how many of those a write barrier forced, bytes promoted, window
  copies and the bytes they moved, ring turnovers, thunks forced. They are
  counted **in place** at a fixed address rather than in globals copied out at
  exit — the first version did the latter and read as all zeros for a run
  stopped by the cycle budget, which is the one run worth measuring. Every
  counter is on a cold path, so none of them is in the dispatch loop.
- **A PC-page profiler** (`A2MIN_PROFILE=1`): instructions per 256-byte page of
  bank 0, top pages printed. On its own a page was the finest honest unit, and
  it was not fine enough: "49% at the tail of bank 0" turned out to be four
  ORCA runtime helpers sharing one page.
- **A function profiler** (`A2MIN_PROFILE=1 A2MIN_SYMBOLS=target/build/cvm.map`):
  cycles per function, and calls into each function by caller, read from the
  JSL return address. `orcalink` always passes the linker `+S`, which does not
  change the load file (checked byte for byte), and `orcabuild` has `omf2bin
  --syms` turn that table into `ADDRESS NAME` lines next to the `.bin`. Its
  first run named `~LSHR4`, ORCA/C's 32-bit shift, at 28% of foldlStrict
  (STATUS.md item 4e).
- **A bus log** (`A2MIN_BUSLOG=1`): DATA-port bytes read per card address,
  watched from the bus, so it needs nothing from the program. The phase
  counters say how many window copies; this says which objects they were.

Bound a long run with **`A2MIN_BUDGET`, never `timeout`**: a budgeted run prints
its counters, DIAG and profile, and a killed one prints nothing.

## Measured (first cut, 2026-09-22)

The C VM against the assembly VM it was translated from, same fixtures, same
harness, CPU cycles for the whole run:

| Fixture | assembly VM | C VM | ratio |
|---|---:|---:|---:|
| `eval-okay-tail-call` (400 iterations) | 2,778,955 | 6,557,612 | 2.4× |
| `eval-okay-lists` | 18,154 | 50,870 | 2.8× |
| `eval-okay-tryeval` | 68,299 | 172,215 | 2.5× |
| binary size | 11,248 B | 38,478 B (25,904 core + 12,552 primops) | 3.4× |

So the C is about **2.5× slower and 3.4× larger** than the assembly, for the
same answers on every fixture (and under a collection at every boundary). That
is the price of D18, paid knowingly: the assembly's last two days were spent on
clobbered scratch registers and branch ranges, not on Nix. The obvious costs are
ORCA/C's calling convention (no inlining; every helper call is a Pascal-style
frame) and 32-bit pointer arithmetic on the bytecode pointer, and the obvious
remedies are the ones in "What stays assembly": a direct-page-resident dispatch
loop and `FORCE`. They are gated on the same rule as everything else — measure,
then change.

What the first optimisation pass found, on the tail-call fixture: turning
`push`/`pop`/`peek`/`fetch` into macros and inlining the collector's trigger
took 9.49M cycles to 6.52M (−31%); optimizer bit 3 (no stack-repair code) a
further 3% and −4 KB; `deref` as a macro at its ~200 call sites cost 12 KB of
code for no measurable speed, so it stayed a function.

## How the port is verified

The assembly VM's tests became the C VM's tests, unchanged:

- `a2min --run-nbc <cvm.bin> <fixture.nbc>` (and `--run-nbc-gc-stress`) loads
  the image into bank 3 and, on an error, prints the DIAG block `main.c` writes
  (pc, the stack depths, `nfree`) and the top of the value stack.
- `scripts/scoreboard.py` runs the corpus and the fixtures;
  `tests/corpus/expected-pass.txt` is the ratchet. While the C VM climbed it had
  a ratchet of its own; at parity that file replaced the assembly's.
- `checks.scoreboard` gates it, including every fixture under GC stress and the
  planted-regression self-test. a2min's cycle budget (50M) is raised for the
  stress run (`A2MIN_BUDGET`): with a collection at every bytecode boundary the
  tail-call fixture needs ~90M.

**Status (2026-09-22): parity reached.** The whole assembly VM — dispatch,
forcing, environments, calls, `with`, strings, lists, sets, equality, deep
force, the 48 primops, `tryEval`, the collector — was translated in one pass;
the fixtures found one bug (see `main.c`: the load file's entry point is the
first function of the first object, and `err_exit` was first). Then 39/39
fixtures, 39/39 with a collection at every bytecode boundary, and **247/405 on
the corpus** against the assembly's 246: the 64 KB nursery lets
`eval-okay-ind-string` finish. (Slice 4 then added a fixture of its own: 248/406.)

**Slice 4 (2026-09-22): promotion to the card.** The collector's step C, the
write barrier, mature reads through the window ring and `LOOKUP` on mature sets,
all in C (`gc.c`, `data.c`, `card.c`), translated from the `promote.s` and
`card.s` that slice 3 left behind. Measured with a collection — now a
promotion — at every bytecode boundary, which puts every object of a program
on the card within one instruction of its creation:

| Fixture | normal: instructions / cycles / bus accesses | every boundary promotes |
|---|---|---|
| `eval-okay-lists` | 13,596 / 54,457 / 1 | 117,859 / 523,530 / 1,116 |
| `eval-okay-attrs` | 49,697 / 200,616 / 1 | 400,243 / 1,784,154 / 3,181 |
| `eval-okay-with` | 34,048 / 137,638 / 32 | 330,324 / 1,477,133 / 2,617 |
| `eval-okay-tryeval` | 45,402 / 186,792 / 1 | 430,160 / 1,927,947 / 3,512 |

Slice 4 cost 4,276 bytes of code (38,478 → 42,754: 30,066 in bank 0, 12,688
of primops in bank 4) — the barrier, the translation pass, the three new
verbs and the window ring bookkeeping (the ring itself is bank 5). Bank 0 now
ends at $9572, so about 10 KB of it are left before the next thing has to become
its own segment.

Turning the window ring over at bytecode boundaries only — the rule that makes
a view's lifetime a whole instruction, HEAP.md “How long a view lives” — costs a
compare and a branch per dispatch, which the four fixtures above put at ≈0.5%
(54,160 → 54,457 cycles on lists, 199,670 → 200,616 on attrs). Bus accesses did
not move at all, in either column: these programs never fill 16 KB of ring, so
they never turn it over and the copy cache is untouched. What that means is that
the fixtures do NOT measure the one thing the rule could plausibly cost — more
frequent turnovers re-copying objects the old ring would still have held — and
the corpus's larger tests are where to look for it (item 8).

The CAM's one correctness hazard has a fixture of its own:
`eval-okay-hash-collision` uses two attribute names with the same djb2 hash
(`getxolul`, `wswmfewd`), so `LOOKUP` matches both and returns the lower index —
the wrong attribute, since names sort `getxolul` first. Getting 2 rather than 1
out of it is proof the CPU-scan fallback ran. It only reaches that path once the
set is mature, so the fixture earns its keep under `--run-nbc-gc-stress`.

Every fixture answers correctly either way. In the normal runs the 64 KB nursery
never fills on a fixture, so the bus sees only the card probe and the multiply
verb; the corpus's larger tests are where promotion happens by itself. Next: the
2.5× measured above.
