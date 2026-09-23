# The VM in C (D18)

The bytecode VM, ported from `target/vm/*.s` to C compiled by ORCA/C
([TOOLCHAIN.md](TOOLCHAIN.md)). [BYTECODE.md](BYTECODE.md) says what it executes,
[HEAP.md](HEAP.md) how values are laid out; this page says how the C program is
shaped, where it lives, and how the port is verified. The assembly VM stays in the
tree until the C one passes every test it does.

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
  $0400  C stack, growing down from $1FFF   7 KB: frames and direct-page locals
  $0BF0  GC_STRESS                       a2min sets it; the collector runs at every boundary
  $2000  the program                     code + globals, from omf2bin; ≤ $BFFF
         globals: value stack (4 KB), RSTACK (768 B), FSTACK (512 B), CATCH (12 × 16 B),
                  CTAB (2 KB), CHUNK_INFO (128 × 4 B), the copy windows (4 × 256 B)
  $C000  never used: slot I/O, the card at $C0nX, then language-card RAM/ROM

bank 1  NURSERY   $010000-$01FFFF   64 KB, bump-allocated
bank 2  STAGE     $020000-$02FFFF   the collector's to-space
bank 3  the .nbc image               $030000, up to 64 KB (a2min --run-nbc-c puts it there)
bank 4+ further code segments        ORCA/C `segment "prims";` etc., placed by omf2bin;
                                     the interpreter core stays in bank 0
```

Bank 0's 40 KB were never going to hold the parser and the store layers, so
code lives in load segments from the start: `prims.c` is `segment "prims"` and
sits in bank 4; the core (`main`, `run`, `data`, `gc`, `card`) and every global
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

## What stays assembly

`asm { }` blocks, in the C source, for exactly three things:

1. **The card's ports** — 8-bit stores to `$C0nX` with `sep #$20`, as `card.s`
   does; a 16-bit C store would write the next register too.
2. **`memcpy`-shaped loops the collector runs** — `mvn` for the copy-back and the
   staging copy; C would loop a byte at a time.
3. **The dispatch loop's opcode fetch**, if measurement says the C version is
   slower than a `jmp (table,x)`; until measured, it is a `switch`.

Everything else is C. The port is a translation, not a redesign: each `op_*`
routine in the assembly becomes a `case`, each helper a function with the same
name, so the two can be read side by side while both exist.

## Layout of the source

```
target/vm/c/
  vm.h        object structs, tags, error codes, the register globals, RSTACK API
  opcodes.h   OP_* / PRIM_* — the same table as opcodes.inc and opcodes.rs
              (checks.nbc-opcodes diffs all four against BYTECODE.md)
  main.c      entry (first in the link: omf2bin takes the first code segment's
              entry), the loader, result reporting, DIAG
  run.c       run(): dispatch, environments, calls, thunks, FORCE, catch frames
  data.c      strings, lists, attribute sets, hashing, equality, deep force
  prims.c     the 48 primops
  gc.c        the Cheney collector into bank 2, copy-back (later: promotion)
  card.c      the driver: ALU64 now; RESERVE, DATA, LOOKUP with slice 4
```

`mkOrcaBinary { name = "cvm"; srcs = [ main.c run.c data.c prims.c gc.c card.c ]; }`
builds it; `just cvm` does the same in the tree.

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

The assembly VM's tests are the C VM's tests, unchanged:

- `a2min --run-nbc-c <cvm.bin> <fixture.nbc>` (and `-gc-stress`) is `--run-nbc`
  with the image at bank 3 and the DIAG block printed instead of the assembly
  VM's zero page. The printer, the result contract and the error names are shared.
- `scripts/scoreboard.py --mode c` runs the corpus and the fixtures on it.
  `tests/corpus/expected-pass-c.txt` is the C VM's own ratchet while it climbs;
  when it reaches the assembly VM's set, one file replaces the other and the
  assembly VM is deleted.
- `checks.cvm-scoreboard` gates it, including every fixture under GC stress.
  a2min's cycle budget (50M) is raised for that run (`A2MIN_BUDGET`): with a
  collection at every bytecode boundary the tail-call fixture needs ~90M.

**Status (2026-09-22): parity reached.** The whole assembly VM — dispatch,
forcing, environments, calls, `with`, strings, lists, sets, equality, deep
force, the 48 primops, `tryEval`, the collector — was translated in one pass;
the fixtures found one bug (see `main.c`: the load file's entry point is the
first function of the first object, and `err_exit` was first). Then 39/39
fixtures, 39/39 with a collection at every bytecode boundary, and **247/405 on
the corpus** against the assembly's 246: the 64 KB nursery lets
`eval-okay-ind-string` finish. `tests/corpus/expected-pass-c.txt` is the
ratchet; `checks.cvm-scoreboard` gates it. Next: slice 4's promotion — the step
that was in progress when the language changed, with `target/vm/promote.s` and
`card.s` as its reference — and then the 2.5× measured above.
