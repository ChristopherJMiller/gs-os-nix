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
  $0340  STATS                           the phase counters, live
  $0400  free                            the C stack's until 2026-10-27 (D31); a2min's
                                         tripwire ends a run that writes here
  $2000  the program                     omf2bin's stub, main.c, every global, the runtime
                                         routines: 16 KB, every symbol in target/vm/bank0.txt
         globals: value stack (4 KB), RSTACK (320 × 4 B), FSTACK (256 × 4 B), CATCH (12),
                  CTAB (2 KB), CHUNK_INFO (256 × 4 B), the window table (64 sets × 2 ways × 8 B)
         ...                             bank 0's room to grow, up to the floor
  $7000  STACK_FLOOR                     nothing at or under it is the C stack's
  $BFFF  C stack, growing down           20 KB: frames and direct-page locals, checked
                                         at STACK_MIN ($7400)
  $C000  never used: slot I/O, the card at $C0nX, then language-card RAM/ROM

bank 1  NURSERY   $010000-$01FFFF   64 KB, bump-allocated
bank 2  STAGE     $020000-$02FFFF   the collector's to-space
bank 3  the .nbc image               $030000, up to 64 KB (a2min --run-nbc puts it there)
bank 4+ further code segments        ORCA/C `segment "prims";` etc., placed by omf2bin,
                                     which skips banks 5 and 7 as the banks fill: `core`
                                     (run.c) in bank 4, `prims` in 6, `float`, `json`,
                                     `regex` in 8, `toml`, `hashes`, `gsos` in 9
bank 5  WINDOWS   $050000-$05EFFF   the 60 KB copy ring: views of mature objects
        RSET      $05F000-$05F9FF   the remembered set: 256 × (object, offset, scratch)
bank 7  SCRATCH   $070000-$07FFFF   a primop's while it runs no bytecode: the regex
                                     engine's NFA, captures, matches and stack
                                     (REGEX.md), fromTOML's document and tree
                                     (TOML.md), listToAttrs' positions (HEAP.md),
                                     the file builtins' paths (GSOS.md)
bank 13 SOURCE    $0D0000-$0DFFFF   the on-target compiler's input text (COMPILER.md)
bank 14 FRONT     $0E0000-$0EFFFF   its working memory: the lexer's contexts
bank 15 FRONT_OUT $0F0000-$0FFFFF   a2min --lex's token records
                                     (omf2bin places code only below bank 13)
```

`$0332` is a third harness flag: `A2MIN_RSET_LIMIT` shrinks the remembered set
so the tests can make it fill. And the GC-stress byte at `$0330` is a period,
not a switch: `A2MIN_GC_PERIOD=k` collects at every *k*-th boundary. At `k` = 1
every value is promoted before it can be stored young, so the remembered set
is never used; `k` = 2 or 3 is what tests it (DECISIONS.md D20).

Bank 0's 40 KB were never going to hold the parser and the store layers, so
code lives in load segments from the start: the primop files are `segment "prims"`,
and since 2026-10-27 `run.c` is `segment "core"` -- 10 KB out of bank 0, at no
cost in cycles (ORCA/C calls everything with `JSL` and reaches globals through
the data bank, wherever the code is: measured, every benchmark to the cycle).
`data`, `gc` and `card` followed when the C stack moved over the program
(D31): -13,547 bytes, again no cycles. What is left in bank 0 is `main.c`,
whose first function is the load file's entry point, every global and static,
and the runtime routines ORCA/C links: `c-lint` refuses a VM source other than
`main.c` with a function outside a `segment` (`scripts/hostlint/segments.py`),
and `scripts/bank-gate.py` keeps a ledger of every bank-0 symbol and its size
in `target/vm/bank0.txt`, so the commit that grows bank 0 says which symbol
did. `omf2bin` refuses a bank-0 segment past `$C000` rather than
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
A fifth is the card's: live mature data must fit its half less a promotion's
room, ~439 KB in the harness (HEAP.md "Collecting mature space"); until
2026-10-27 nothing collected mature space, and a program could promote 512 KB
in all. A derivation adds its own: its final ATerm, and
everything else its one reserve covers, must fit the nursery (DERIVATION.md
"Ceilings").

The C stack is the 65816 stack: ORCA/C allocates a function's locals below the
stack pointer and points the direct-page register at them (`tsc / sbc / tcs / phd
/ tcd`), so `FORCE`'s recursion through `run()` costs one C frame per nesting
level, as the assembly's did through `jsr run`. `E_NOMEM` when it nears its floor.

### The C stack

The stack is bank 0's top (since 2026-10-27, D31): omf2bin's stub starts S
at `STACK_TOP` (`$BFFF`, under the I/O page), and nothing at or under
`STACK_FLOOR` (`$7000`) is the stack's -- under it is bank 0's room to grow,
then its globals. `stack_check()` -- at every force and call -- refuses
(`E_NOMEM`) once S is under `STACK_MIN` (`$7400`), which is only safe if
every path from one check to the next fits in the margin `STACK_MIN` leaves
above the floor.

Until then it ran down from `$1FFF` to `$0400`, under the program, over the
harness's `RESULT`, `DIAG`, flags (`$0330`) and `STATS` (`$0340`), and **the
margin was not enough** (found 2026-10-27, building the speed benchmarks): it
was 256 bytes, and a chain of seven derivations, each a level of
`derivationStrict` (a 486-byte frame) under the previous, took S to `$0351`.
That wrote over the flags -- GC stress every sixth boundary and three verbs
masked, from then on -- and over `STATS` (80 MB "promoted") before any check
saw it; the run then failed as `E_NOMEM`, for the wrong reason. Measured
over every fixture and corpus test since, the deepest stack used between
two checks (a2min, from each check's S to the lowest S before the next):

| Path | Bytes | |
|---|---|---|
| `fromTOML`, 32 nested arrays and a float | 2,538 | `tm_parse`, then `f_strtod` |
| `fromJSON` numbers, `toXML` floats | 1,284, 1,225 | `f_strtod` 1,068, `g_digits` 1,038 |
| float `toString`, `toJSON` floats | ~980 | `f_format` 502, `f_json` |
| a fixed-output derivation | 682 | `prim_derivationstrict` 486 |
| `hashString` | 665 | `sha512_block` 238, `str_hash` 218 |
| everything else | under 560 | |

(The frame sizes are read from each function's prologue, `TSC SEC SBC #n
TCS`, in `cvm.bin`.) So `STACK_MIN` is the floor and 1 KB: a margin over the
682 bytes of the deepest ordinary path. The paths through `float.c` and `toml.c`,
which have no VM in them and so no checks, call `stack_need(n)` first, at
each of their five VM-side entries: `FLOAT_STACK` (1,600) and `TOML_STACK`
(2,600) bytes above `STACK_MIN`. The cost is 1 KB of stack -- a derivation
nests about 1 KB deep a level -- for a margin that is now measured, not hoped.

**How deep a program can nest**, measured -- the deepest each kind of
nesting answers at before `E_NOMEM`, with the 6 KB the stack had over its
check under the program, and the 19 KB it has now:

| Nesting | C stack a level | Before | Now | What ends it |
|---|---|---|---|---|
| a call in an operand, `1 + f (n - 1)` | ~72 bytes | 83 | 270 | the C stack |
| a walk down nested sets | ~73 | 81 | 269 | the C stack |
| a thunk forced inside a thunk (a lazy `foldl`) | ~130 | 46 | 151 | the C stack |
| `deepSeq` of lists in lists | ~21 | 188 | 316 | the root stack |
| a chain of derivations | ~1,100 | 5 | 17 | the C stack |

Each level also holds a root (`rstack`, 4 bytes) and, for a thunk, a force
(`fstack`), so those grew with the stack -- `RSTACK_N` 192 to 320 and
`FSTACK_N` 128 to 256, +1 KB of bank 0 -- to keep the C stack what ends the
costly shapes. (The old 192 roots was already the next wall: with the bigger
stack alone, calls stopped at 190.) `deepSeq` spends so little C stack a level
that ending it there too would take ~900 roots, 3.6 KB more of bank 0.
`eval-okay-deep-nesting` takes the first four to about nine-tenths of these,
and `eval-okay-derivation-chain` the last to 16; two mutations put the old
array sizes back.

Two things hold the floor: a2min's tripwire, which ends any VM run as "C
stack overflow" if its S goes under the floor (outside page 1, the caller's
stack) or it writes to bank 0 that is nobody's -- the flags, the gap under
them, the old stack's `$0400-$1FFF`, and the growth room -- in every run of
every check; and `scripts/stack-stress.sh` (`just stack-stress`,
`checks.stack-stress`), which runs the deepest program of each kind with its
stack starting at every 64-byte step from over its own depth (a2min prints
every run's lowest S; the start adds the largest `stack_need`) down to
`STACK_MIN` (`A2MIN_STACK_TOP` patches omf2bin's stub), so that each check
meets the floor somewhere. Every run must give the answer or `E_NOMEM`. A VM
built with the old 256-byte margin must fail it, and does: it writes `$6FFF`.
`scripts/bank-gate.py` holds bank 0's end under the floor, and the stub's S
and a2min's two numbers to `vm.h`'s.

### Speed (D29)

`scripts/bench.py` (`just bench`, `checks.bench`) runs six programs in
`tests/bench/`, each a different kind of work, once each with the card, and
holds their CPU cycles to `tests/bench/cycles.txt`: an answer must be the
host's (the `.exp` is `nix-instantiate`'s), and a cycle more than recorded
fails. a2min is cycle-exact, so the ratchet is exact. Faster is recorded with
`just bench-update`, in the commit that made it so. The baseline
(2026-10-27):

| Benchmark | Cycles | Its work |
|---|---|---|
| `tail-calls` | 99,949,902 | 5,000 calls of `n: if n == 5000 then n else f (n + 1)` -- 20,000 a call |
| `recursion` | 77,692,598 | `fib 15`, `map`/`filter` and `sort` over 300 |
| `strings` | 65,927,931 | 800 interpolations, a join, a slice, a SHA-256 |
| `foldl-strict` | 45,947,756 | `foldl'` over 1,000 |
| `attrs` | 36,252,861 | `listToAttrs` of 200, a `//`, 200 computed selections |
| `derivations` | 10,532,760 | four chains of three derivations, to the `.drv` paths |

Where `tail-calls`' cycles go (`A2MIN_PROFILE`, 4,000 calls): `deref` 26% --
26 calls an iteration, ~196 cycles each, most of it ORCA/C's call and frame
overhead around a two-line function -- `run` itself 24%, `mature_view` 8%
(5.6 views an iteration, of the lambda and the constants, which the first
collection promoted), `force_top` 8%, `force_thunk` 5%, `values_equal` 5%.
The levers, in order: `deref`'s young case inline at the hot sites, and
`force_top`'s already-forced case; keeping the constants out of the nursery,
where promotion makes every `CONST` a view; then the dispatch loop.
Each is measured here and priced in bank-0 bytes.

**Step 1 (2026-10-27): 336.3M -> 290.8M cycles, -13.5%**, and the full
`eval-okay-tail-call-1` 2.13G -> 1.73G (`tail-calls`
-21.5%, `recursion` -16.7%, `foldl-strict` -8.6%, `strings` -6.7%, `attrs`
-6.4%, `derivations` -4.0%):

| Change | All six | Bank 0 |
|---|---|---|
| `DEREF`, `deref`'s young case inline, at `run.c`'s hot sites | -7.6% | +409 |
| the window cache's hit tested in `deref`, not a call to `mature_view` | -2.2% | -1 |
| `FORCE_TOP` / `FORCE_SLOT2`: no `force_top` call for a value | -4.1% | +1,321 |
| `run.c` into `segment "core"` | 0 (to the cycle) | -10,235 |

The forcing check first cost 2 KB, because `NEEDS_FORCE` named its argument
twice and so expanded `DEREF` twice: a macro's argument is read into a local.
Moving `run.c` out of bank 0 changes nothing the CPU does -- ORCA/C calls with
`JSL` and reaches globals through the data bank wherever code is -- and pays
for every fast path here several times over. Two mutations guard the fast
paths (an `INDIRECT` left unfollowed, a mature pointer taken for a view).

**Step 2 (2026-10-27): 290.8M -> 263.8M cycles, -9.3%** (-21.6% from the
start), and `eval-okay-tail-call-1` 1.73G -> 1.37G. Bank 0 +207.

| Change | All six | Notes |
|---|---|---|
| the GC-stress period read once at entry, not through a long pointer at every boundary | -1.4% | |
| `==` of two INTs inline in `EQ` | -3.3% | `tail-calls` -11.6%: `values_equal` cost ~1,800 cycles for two INTs |
| `<` `>` `<=` `>=`: two INTs first in `cmp_values`, ordered from the bytes | -0.3% | no `~SCMP8`, the 64-bit signed compare, a call |
| `+` `-` of two INTs inline in the dispatch | -1.6% | `add_checked`/`sub_checked` still: Nix 2.34's overflow is an error |
| `ALLOC`, `alloc` inline, in `new_int`, `closure`, `call_enter` | -1.6% | |
| `win_slot`'s shift unsigned | -1.0% | an `int`'s `>>` is `~SSHIFTRIGHT`, a call, on every mature `deref` |
| the boundary's GC test as branches | -0.6% | ORCA/C made `gc_due()`'s `?:` a boolean, then tested it |

Measured on the way (a2min trace, per dispatch, `tail-calls`): the loop
alone is ~30 65816 instructions an operation -- `POP`, whose body is
`--vsp`, costs 34 -- and `CONST` 93, `ENV_GET` 156, `EQ` 272, `ADD` 414.
`fetch8` is nine instructions through a 32-bit `pc` in memory. Four
mutations guard the integer paths; two of them survived every fixture until
`eval-okay-int-compare`, which compares INTs differing only in their high 32
bits and only in sign. And `c-lint`'s orcabugs refused the first `+`/`-`:
`op == OP_ADD ? add_checked(...) : sub_checked(...)` is a 64-bit `?:` with a
call in an arm, the shape ORCA/C miscompiles (TOOLCHAIN.md) -- an `if` now.

**Step 3 (2026-10-27): 263.8M -> 210.2M cycles, -20.3%** (-37.5% from the
start), and `eval-okay-tail-call-1` 1.37G -> 1.04G -- half what it was. The
pattern under most of it: ORCA/C's 32-bit code. A test or a store on a
pointer or a `uint32_t` as a whole is 12 to 20 instructions; on its two
16-bit halves, two to four. Read from a small 65816 disassembler over
`cvm.bin` (a scratch tool) and a per-dispatch count from a2min's trace.

| Change | All six | Notes |
|---|---|---|
| `fetch16` one 16-bit load | -2.5% | the two-byte form recomputed the address a byte |
| `ctab` an index into `ctab_pool`, not a pointer | -0.7% | `CONST` an absolute indexed load |
| `force_thunk`'s young paths inline (blackhole, the value's view) | -1.0% | |
| `IS_NULL` (a pointer's halves ORed) in `ENV_GET` | -0.2% | |
| `IS_RAM` on the pointer's high word | -3.5% | its 32-bit `&` was ~20 instructions, in every `deref` |
| `SET_HDR` and `ALLOC`'s young pointer by halves | -2.1% | `YOUNG_PTR`'s ORs went through the stack |
| no divide in `nursery_reserve_n`'s check, `toString` below 2^32, djb2 | -1.6% | powers of ten as a string literal; `h * 33` by halves |
| `insert_entry_pos` by binary search | -3.1% | `attrs` -24%: it was a scan from the start, a `str_cmp` a name |
| the parameter port in bursts (`ALU64`'s operands, `LOOKUP`'s key, the digest) | -4.0% | a `cw8` call a byte was ~76 cycles for one access |
| constants in the image bank, above the image | -3.9% | never promoted, so never a view; the nursery when the bank is full |

Tried and taken out: stepping `pc` by its low word only (the image is one
bank). ORCA/C turned `((uint16_t *)&pc)[0] += 2` into a long pointer to `pc`
on the direct page and a read-modify-write through it -- 0.7% slower.
Six mutations (mutation-vm 258 -> 264); `eval-okay-tostring-powers` holds every power of ten's
edge, and `eval-okay-big-constants` both the image bank and the fallback.
The binary search also made an older mutation decorative -- the window-ring
turnover between `listToAttrs`' insertions, which `eval-okay-big-operations`
had needed when every insertion scanned every name -- so
`eval-okay-listtoattrs-long-names` (100 promoted names of ~700 bytes: 70 KB
of views in one instruction) now needs it, and catches its removal.

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
              -- segment "core", bank 4
  data.c      strings, lists, attribute sets, hashing, equality, deep force,
              the write barrier and the window ring for mature objects
  prims.h     what the three primop files share (private to them)
  prims.c     the primops' shared helpers, lists, sets, errors, the table -- bank 6
  strings.c   string primops, coercion, interpolation, string context -- bank 6
  derivation.c  derivationStrict, the ATerm, fixed outputs, and the
              derivation rule of equality (DERIVATION.md) -- bank 6
  gc.c        the Cheney collector into bank 2, then promotion to the card
              (copy-back into the nursery when no card is present)
  card.c      the driver: ALU64, RESERVE, DATA, LOOKUP, the SHA-256 verbs
  sha256.c    SHA-256 and the store-path fold on the CPU, for no card -- bank 6
  float.c     IEEE-754 binary64 on bit patterns, and %f: no VM in it, so
              checks.float-vectors tests it alone (FLOAT.md) -- segment "float"
  numbers.c   the VM's floats: FLOAT objects, mixed INT/float arithmetic and
              comparison, floor, ceil, toString -- segment "float"
  json.c      builtins.toJSON (Nix's tree, then nlohmann's text),
              builtins.fromJSON (nlohmann's lexer and parser) and
              builtins.toXML (Nix's XMLWriter) -- segment "json"
  regex.c     libstdc++'s std::regex, ported: its scanner, compiler, NFA and
              depth-first executor, on bank 7; no VM in it (REGEX.md) --
              segment "regex"
  match.c     builtins.match and builtins.split on it -- segment "regex"
  toml.c      toml11's TOML 1.0 parser, ported, on bank 7; no VM in it
              (TOML.md) -- segment "toml", in bank 9
  hashes.c    hashString's md5, sha1 and sha512 on the CPU, their tables
              string literals -- segment "hashes", in bank 9 after toml
  gsos.c      pathExists, readFile, readFileType, readDir, hashFile, getEnv
              through GS/OS's stack entry, Nix's symlink walk on bank 7
              (GSOS.md) -- segment "gsos", in bank 9 after hashes
```

`core` is in bank 4, `prims` fills bank 6, `float`, `json` and `regex` are in
bank 8, and `toml`, `hashes` and `gsos` in bank 9: omf2bin skips bank 5, the
window ring, and bank 7, the scratch bank.

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
