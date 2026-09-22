# Bytecode format (`.nbc`) — normative

The 65816 VM (`target/vm/`) executes `.nbc` files. This document defines the file
format, the constant pool, the opcode set, and — the part that matters most for a
lazy language — **where thunks are placed**. The host-side generator (`tools/nbc/`)
and the VM must agree with it byte for byte; `checks.nbc-opcodes` asserts that the
opcode table below, the generator's `opcodes.rs` and the VM's `opcodes.inc` are the
same table.

## Where `.nbc` files come from, and where they never go

Per [DECISIONS.md](DECISIONS.md) **D7**, `.nbc` files are **test fixtures only**. The
host generator exists so the VM can be built and measured before the on-target
compiler (L5) exists; at L5 the same format is produced by the 65816, the host
generator becomes the differential-test oracle for that compiler, and at M3 the
host is out of the loop entirely. Nothing at runtime ever reads a host-produced
`.nbc`.

`checks.fixture-purity` enforces the corollary: the generator contains no hashing
or store-path code, and no `.nbc` it emits contains a store path or digest that was
not already a literal in its `.nix` source. A fixture that smuggled a precomputed
answer would turn a parity milestone into a tautology.

## Shape: tvix's, with two 65816 adjustments

The design is **tvix's** — a stack VM, a constant pool and code per chunk, thunks
created by an explicit opcode at compile-time-chosen sites, and forcing with
in-place update — because tvix has already worked out which laziness decisions Nix
semantics actually require, and its conformance corpus is the scoreboard.

Two things are done differently, both for the same reason: the 65816 has three
registers and no cheap way to copy words around.

1. **Environments are heap objects, not stack slots.** tvix resolves every local to
   a stack index at compile time and *copies* captured values into each closure and
   thunk it creates. Creating a thunk is the hottest allocation in lazy evaluation,
   so here a thunk captures **one pointer** — the current environment — and the
   body reads variables with `ENV_GET depth, index`. A `let` allocates one `LIST`
   for all its bindings; the thunks share it. Forward and self references inside a
   recursive `let` need no fix-up pass (tvix's `OpFinalise`), because every thunk
   sees the slot, not a copy of what was in it.
2. **Operands are fixed-width little-endian.** Indices are `u16`, jump offsets are
   `i16`, integers are 8 bytes. No varints — a 65816 decodes a `u16` with one
   16-bit load and a varint with a loop.

Everything else — the strict/lazy split per syntactic position, blackholing during
evaluation, `if` compiled to a peeking conditional jump, chunk-per-thunk — follows
tvix.

## File layout

All integers little-endian.

```
Header (16 bytes)
  +0   "NBC"          3 bytes, magic
  +3   version        u8   = 1
  +4   chunk_count    u16
  +6   entry_chunk    u16  index of the chunk to evaluate (always 0 today)
  +8   total_size     u32  bytes in the file, header included
  +12  reserved       u32  = 0

Chunk table
  chunk_count x u32   byte offset of each chunk from the start of the file

Chunks, in index order
  +0   const_count    u16
  +2   code_len       u16  bytes of code
  +4   constants      const_count entries, see below
  ...  code           code_len bytes
```

The chunk table is what lets `THUNK idx` find its body without walking the file. A
fixture must fit in one 64 KB bank; the VM refuses a `total_size` it cannot load
rather than truncating.

### Constants

A constant is a `u8` kind followed by a payload. **Kinds are heap tags** from
[HEAP.md](HEAP.md), so the loader materialises each constant into a heap object by
copying the tag straight into the header:

| Kind | Tag | Payload |
|---|---|---|
| `$01` | `INT` | 8 bytes, signed |
| `$04` | `STRING` | `u16` length, then the bytes (no terminator) |

Booleans and `null` are not constants: they are opcodes that push shared singleton
objects. Chunks (thunk and lambda bodies) are not constants either — `THUNK` names
its body by chunk index directly.

Constants are materialised **once, at load time**, into a per-chunk pointer table;
`CONST idx` pushes the pointer. Two evaluations of the same literal therefore share
one object, which Nix semantics permit (values are immutable) and which keeps
integer-heavy code from allocating.

## The VM's registers and stacks

| | |
|---|---|
| **value stack** | 32-bit pointers, in bank 0, separate from the CPU stack. `sp` is a direct-page word |
| **env** | direct-page pointer to the current environment `LIST` (see below); `$00000000` at top level |
| **frames** | the CPU's own stack: `FORCE` pushes the saved `env` and return address there, since the 65816 does that in two instructions |

A **value** is a 32-bit pointer to a heap object. Pointers with bit 31 set point
into the nursery (CPU RAM); the rest point at the card. Slice 1 of the VM allocates
only in the nursery.

An **environment** is an ordinary `LIST` object whose element 0 is the parent
environment (or null) and whose elements `1..n` are the bindings, so
`ENV_GET depth, i` walks `depth` parents then reads element `i`. Being an ordinary
`LIST`, environments are traced and promoted by the collectors without special
cases — which is the other reason not to keep locals on a stack.

## Opcodes

Operands follow the opcode byte in the order listed. Stack effects are written
`before → after`, top on the right. "Forces" means the operand is a value after the
instruction even if it was a thunk before (in-place update, see below).

| Op | Name | Operands | Stack | Semantics |
|---|---|---|---|---|
| `$00` | — | | | **illegal.** The VM aborts with `E_BADOP`. `$00` is what uninitialised memory looks like, so it must never be a valid instruction |
| `$01` | `CONST` | `u16 idx` | `→ v` | push the chunk's materialised constant `idx` |
| `$02` | `POP` | | `v →` | |
| `$03` | `TRUE` | | `→ v` | push the shared `BOOL 1` |
| `$04` | `FALSE` | | `→ v` | push the shared `BOOL 0` |
| `$05` | `NULL` | | `→ v` | push the shared `NULL` |
| `$10` | `ADD` | | `a b → a+b` | forces both. `INT` only in slice 1 (strings and paths later). On the CPU: four 16-bit adds |
| `$11` | `SUB` | | `a b → a−b` | forces both. On the CPU |
| `$12` | `MUL` | | `a b → a×b` | forces both. **`ALU64 MUL` on the card** |
| `$13` | `DIV` | | `a b → a÷b` | forces both. **`ALU64 DIV`**; truncates toward zero; `b = 0` → `E_DIVZERO` |
| `$14` | `NEG` | | `a → −a` | forces. On the CPU |
| `$16` | `EQ` | | `a b → bool` | forces both. Slice 1: `INT`, `BOOL`, `NULL`; different types compare unequal |
| `$17` | `LT` | | `a b → bool` | forces both. Signed 64-bit, **on the CPU**: the top word compared signed (N⊕V after `SBC`), the lower three unsigned — ~40 cycles, so `ALU64 CMP` (32 bus accesses) does not pay and D5 forbids it |
| `$18` | `LE` | | `a b → bool` | as `LT` |
| `$19` | `GT` | | `a b → bool` | as `LT` |
| `$1A` | `GE` | | `a b → bool` | as `LT` |
| `$1B` | `NOT` | | `b → !b` | forces; `BOOL` only |
| `$20` | `JMP` | `i16 rel` | | `pc += rel`, measured from the end of this instruction |
| `$21` | `JMP_IF_FALSE` | `i16 rel` | `b → b` | forces the top; jumps if it is `BOOL 0`. **Does not pop** — the compiler emits `POP` on each path, as tvix does, so `&&`/`||` can keep the value |
| `$22` | `JMP_IF_TRUE` | `i16 rel` | `b → b` | as above, for `BOOL 1` |
| `$23` | `ASSERT` | | `b →` | forces; `BOOL 1` continues, anything else aborts with `E_ASSERT` (or `E_TYPE`) |
| `$30` | `ENV_NEW` | `u16 n` | | `env ← LIST(n+1)` with `[0] = old env`, `[1..n] = null` |
| `$31` | `ENV_SET` | `u16 i` | `v →` | `env[i] ← v` (`i ≥ 1`) |
| `$32` | `ENV_GET` | `u16 depth`, `u16 i` | `→ v` | walk `depth` parents, push element `i`. **Does not force** |
| `$33` | `ENV_POP` | | | `env ← env[0]` |
| `$34` | `LAMBDA` | `u16 chunk` | `→ f` | allocate `LAMBDA { code = chunk, env = env }` — a closure is a thunk with a parameter |
| `$35` | `CALL` | | `f a → v` | forces `f` (must be `LAMBDA`); `env' ← LIST(2)` with `[0] = f.env`, `[1] = a`; runs `f.code` with `env'` until `RETURN`; its value replaces both operands. The argument is **not** forced — it is whatever the caller pushed, usually a thunk |
| `$40` | `THUNK` | `u16 chunk` | `→ t` | allocate `THUNK { code = chunk, env = env }` |
| `$41` | `FORCE` | | `t → v` | see below |
| `$42` | `RETURN` | | `v →` | end of chunk: hand `v` to whoever forced this chunk, or, for the entry chunk, to the harness |

Every gap in the numbering is deliberate room for the next slice (`$06`–`$0F`
constants and strings, `$15` string ops, `$24`–`$2F` attrs/list ops, `$36`–`$3F`
`with` and formals, `$43`+ builtins).

### `FORCE`, blackholing, in-place update

```
FORCE:
  loop:
    v = top
    if tag(v) == INDIRECT:  top = v.target; goto loop
    if tag(v) == THUNK:
        tag(v) = BLACKHOLE                 ; re-entry is infinite recursion
        push CPU frame {env, pc}; env = v.env; run chunk v.code until RETURN
        r = returned value
        if size(r) <= 12: copy r's bytes over v  ; v IS the value now
        else:             v = INDIRECT -> r
        top = v (or r); pop CPU frame
    if tag(v) == BLACKHOLE: abort E_INFREC
    otherwise: done
```

A `THUNK` is 12 bytes (header, code, env). `INT` is 12 bytes, `BOOL` and `NULL` 8,
so every slice-1 result **overwrites the thunk in place** and every other reference
to it sees the value with no indirection. Larger results become an `INDIRECT`,
which is what the card's `FOLLOW` verb chases.

When an 8-byte value overwrites a 12-byte thunk, 4 bytes of slack follow it. That
is safe **because both collectors are Cheney copiers**: nothing ever walks from-space
linearly, sizes are only ever read through a pointer, and to-space is laid out by
the copier itself. [HEAP.md](HEAP.md) records this as the one permitted way for an
object's footprint to shrink.

## Thunk placement (what is lazy)

tvix's rule, kept exactly: an expression is compiled **strictly** if its value is
needed the moment the enclosing expression is, and as a **thunk** otherwise. The
generator makes the decision per syntactic position:

| Position | Compiled as |
|---|---|
| the whole program | strict, then `RETURN` |
| `let` binding whose initialiser is a literal (`CONST`/`TRUE`/`FALSE`/`NULL`) | the literal itself, stored with `ENV_SET` — no thunk, nothing to defer |
| `let` binding whose initialiser is a bare variable bound in an **outer** scope or **earlier** in the same `let` | `ENV_GET` without `FORCE` — a pointer copy; the referenced thunk stays lazy and shared |
| `let` binding whose initialiser is a bare variable bound **later** in the same `let`, or itself | `THUNK` — the slot is still null when this binding is stored |
| any other `let` binding | `THUNK`, whose body ends in `RETURN` |
| body of `let ... in body` | strict (the `let` is being evaluated) |
| operands of `+ - * / == < <= > >= ! -` | strict: compiled inline, then forced |
| condition of `if` | strict; `JMP_IF_FALSE` forces it |
| branches of `if` | strict, inline — only one runs, so neither needs deferring |
| operands of `&&`, `||`, `->` | left strict; right compiled inline behind a conditional jump, which is the same laziness Nix specifies (short-circuit) without a thunk |
| a variable reference | `ENV_GET`, then `FORCE` **only if** the position is strict |
| the function in `f x` | strict — `CALL` needs a `LAMBDA` now |
| the argument in `f x` | **lazy**: a literal directly, a variable as a pointer copy, anything else a `THUNK`. `(x: 1) (1 / 0)` is `1` |
| body of a lambda | strict, in its own chunk, ending in `RETURN`; its scope is one binding (the parameter) whose parent is the closure's environment |
| condition of `assert` | strict; `ASSERT` forces it. The body follows inline |

So `let x = 1 + 2; y = x; in if y > 2 then x else 0` compiles to one thunk (`x`),
no thunk for `y` — its initialiser `x` is a bare variable and `ENV_GET` of an
existing thunk is already lazy — and forces `y` once at the comparison, which forces
`x` once, in place, so the `then` branch's `ENV_GET; FORCE` finds an `INT`.

Recursion through a `let` (`let f = ...; x = f 1; in x`) is what the shared
environment buys: the thunk for `x` captures the `let`'s environment *while `f`'s
slot is still null*, and reads it when forced, by which time `ENV_SET` has filled
it. `let x = x; in x` therefore forces `x`, blackholes it, forces `x` again, and
aborts with `E_INFREC` — which is the Nix result.

## The compiled shape of the slice-1 forms

```
let b1 = e1; ...; bn = en; in body
    ENV_NEW n
    <e1 as literal | THUNK c1>   ENV_SET 1
    ...
    <en as literal | THUNK cn>   ENV_SET n
    <body, strict>
    ENV_POP

if c then a else b
    <c, strict>
    JMP_IF_FALSE L1
    POP
    <a, strict>
    JMP L2
L1: POP
    <b, strict>
L2:

a && b                                a || b
    <a, strict>                           <a, strict>
    JMP_IF_FALSE L                        JMP_IF_TRUE L
    POP                                   POP
    <b, strict>                           <b, strict>
L:                                    L:

a -> b   ≡   !a || b
```

## Result reporting (harness contract)

The entry chunk's `RETURN` value is written by the VM to `$0300`:

```
$0300  tag        1 byte: INT $01, BOOL $02, NULL $03
$0301  payload    INT: 8 bytes signed LE.  BOOL: 1 byte
$0320  status     $01 done; $FF error, with the error code at $0321
```

A `LAMBDA` result is reported by tag alone; the harness prints `<LAMBDA>`, as Nix does.

Error codes: `E_BADOP $01`, `E_TYPE $02`, `E_DIVZERO $03`, `E_INFREC $04`,
`E_NOMEM $05` (nursery, value stack or call/force depth exhausted), `E_BADNBC $06`
(malformed image, or a slot read before it was set — a generator bug), `E_NOCARD
$07` (`MUL`/`DIV` with no card in any slot), `E_ASSERT $08`. The harness (`a2min --run-nbc`) prints the value in
Nix syntax (`42`, `true`, `null`) so the scoreboard can compare it with the corpus's
`.exp` files, and prints `error: <name>` otherwise.

## Slice plan

1. **Integers, booleans, null; `let`; `if`; thunks with in-place update.** Done.
   Gate: `checks.scoreboard` passes every corpus test the generator can compile,
   and the count is in `tests/corpus/expected-pass.txt` so it can only go up.
2. **a. Lambdas, application, `assert`.** Done — `LAMBDA`, `CALL`, `ASSERT`.
   **b.** Strings (with context pointers from day one), lists, attribute sets and
   selection (CPU-side on young sets, `LOOKUP` on promoted ones), `builtins`.
3. `with`, `rec`, formals, inline caches, and **`TAIL_CALL`**: a `CALL` that is the
   last instruction before `RETURN` reuses the frame instead of nesting. Without
   it `eval-okay-tail-call-1` (100 000 calls) exhausts the call stack — and without
   slice 4 it exhausts the nursery, so it needs both.
4. Builtins that touch the store, `import`, paths; the nursery collector runs
   during evaluation.
