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
| `$24` | `LIST` | `u16 n` | `e1 … en → l` | allocate `LIST` of the `n` values in push order. Elements are **not** forced |
| `$25` | `CONCAT` | | `a b → a++b` | forces both; `LIST` only |
| `$26` | `ATTRS` | `u16 n` | `k1 v1 … kn vn → s` | allocate `ATTRS`: names are `STRING` values pushed **in sorted order** by the generator (it refuses duplicates); the VM hashes each name (HEAP.md) into the key column. Values are not forced |
| `$27` | `SELECT` | | `s k → v` | forces `s` (must be `ATTRS`); `k` is a `STRING`; missing → `E_NOATTR` |
| `$28` | `TRY_SELECT` | | `s k → v \| ∅` | as `SELECT`, but a missing attribute **or a non-`ATTRS` `s`** pushes the not-found marker ∅ (the null pointer, never otherwise a value) instead of aborting — Nix's `or` catches both |
| `$29` | `JMP_IF_NOTFOUND` | `i16 rel` | `x → x` | peeks; jumps if the top is ∅. The compiler emits `POP` on the taken path |
| `$2A` | `HAS_ATTR` | | `s k → bool` | forces `s`; `false` if it is not an `ATTRS` |
| `$2B` | `UPDATE` | | `a b → a//b` | forces both; merges by name, `b` wins |
| `$2C` | `INTERPOLATE` | `u16 n` | `p1 … pn → str` | forces the `n` parts; each must be a `STRING` in this slice (Nix coerces paths and `__toString` sets too — later); concatenates. String context is the union — carried from day one as the context pointer, currently always null |
| `$43` | `PRIM` | `u8 id` | `a1 … ak → v` | run primop `id` (table below) on its `k` arguments, which are on the stack unforced; the primop forces what it needs |
| `$36` | `WITH_PUSH` | | `s →` | `env ← LIST(2)` with `[0] = old env`, `[1] = s` (unforced), and the **with mark** (bit 22 of `aux`) set. A `with` scope is an environment like any other, so thunks capture it and `ENV_GET` depths count it |
| `$37` | `WITH_GET` | | `k → v` | walk the environment chain; at each marked environment force `[1]` (must be `ATTRS`) and look `k` up; the first hit is pushed unforced. None → `E_NOVAR` |
| `$38` | `TRY_WITH_GET` | | `k → v \| ∅` | as `WITH_GET`, but a miss pushes ∅ — the compiler uses it when a global (`map`, `toString`, …) is the fallback |
| `$39` | `TAIL_CALL` | | `f a → ` | `CALL` without a frame: sets `pc`/`ctab`/`env` to the callee and continues, so the callee's `RETURN` returns to *this* chunk's caller. Emitted for a `CALL` immediately before `RETURN` |

### Catch frames: `tryEval` and `throw`

`builtins.tryEval e` is primop `$2F` (arity 1, argument unforced). It records a
**catch frame** — the CPU stack pointer, `vsp`, `env`, `pc`, `ctab` — on a small
catch stack in RAM, then forces its argument. If the force returns, the frame is
popped and `{ success = true; value = v; }` is built. If anywhere inside a
*catchable* error is raised — `E_THROW` or `E_ASSERT`, the two Nix lets `tryEval`
catch; not `abort`, not type or missing-attribute errors, which propagate as in
Nix — the error exit sees a non-empty catch stack, pops the frame, restores the
four registers and the CPU stack, and resumes in `tryEval` with
`{ success = false; value = false; }`. Thunks that were being forced when the
error unwound would be left `BLACKHOLE`; the VM keeps a **force stack** of the
thunks under evaluation, the frame records its depth, and the unwind turns every
entry above it back into a `THUNK` — so forcing a failed thunk again re-runs it
and throws again, which is Nix's observable behaviour
(`eval-okay-tryeval-thunk-twice`).

### Desugarings the generator performs

Nix has several forms that are other forms in disguise; the lowering rewrites
them so the VM and the compiler never see them:

| Form | Becomes |
|---|---|
| `rec { a = 1; b = a; }` | `let a = 1; b = a; in { inherit a b; }` |
| `let { a = 1; body = a; }` (legacy) | `(rec { a = 1; body = a; }).body` |
| `{ a.b = 1; a.c = 2; }` | `{ a = { b = 1; c = 2; }; }` — paths are merged at compile time; a path that lands on a non-literal is refused |
| `{ a, b ? d, ... } @ s: body` | `s: let a = s.a; b = s.b or d; in body` — formals are lazy, defaults may refer to other formals, `@` names the whole set. (The "unexpected argument" check on closed formals is **not** yet performed.) |

`ADD` is polymorphic at runtime: two `INT`s add, two `STRING`s concatenate (context
union), anything else is `E_TYPE`. `EQ` compares `STRING`s by bytes, `LIST`s
element-wise and `ATTRS` by name and value (both **deep-forcing**, as Nix does),
and different types compare unequal.

### Primops

`builtins.NAME` — when `builtins` is not shadowed — and the unqualified globals
(`map`, `toString`, `throw`, `abort`, `isNull`, `removeAttrs`, `baseNameOf`,
`dirOf`, …) compile to an ordinary `LAMBDA` whose body is a chunk the generator
synthesises once per program: `ENV_GET` for each argument (outermost first), `PRIM
id`, `RETURN`, nested one lambda per argument. So a primop **is** a function value:
it curries, partially applies, and is passed lazily-thunked arguments like any
other — and the VM needs no primop object type. `builtins.NAME` for a `NAME` not
in this table is refused by the generator, by name, so the scoreboard shows which
primop would move the most tests.

| Id | Name | Arity | Semantics (this slice) |
|---|---|---:|---|
| `$00` | `length` | 1 | `LIST` → `INT` |
| `$01` | `head` | 1 | first element, forced; empty → `E_INDEX` |
| `$02` | `tail` | 1 | new `LIST` without the first; empty → `E_INDEX` |
| `$03` | `elemAt` | 2 | `list i`; out of range → `E_INDEX` |
| `$04` | `map` | 2 | `f list` → new `LIST` of **thunks** `f x` — lazy, as Nix's is |
| `$05` | `filter` | 2 | `f list`; forces `f x` for each |
| `$06` | `foldl'` | 3 | `f z list`; strict accumulator |
| `$07` | `concatLists` | 1 | `[ [..] [..] ]` → flat `LIST` |
| `$08` | `genList` | 2 | `f n` → `[ (f 0) … (f (n-1)) ]`, thunks |
| `$09` | `elem` | 2 | `x list` → `BOOL`, `EQ` semantics |
| `$0A` | `any` | 2 | `f list` |
| `$0B` | `all` | 2 | `f list` |
| `$0C` | `concatMap` | 2 | `f list` |
| `$0D` | `attrNames` | 1 | `ATTRS` → `LIST` of `STRING`s, sorted (the name column as stored) |
| `$0E` | `attrValues` | 1 | `ATTRS` → `LIST` of values in name order |
| `$0F` | `hasAttr` | 2 | `name set` |
| `$10` | `getAttr` | 2 | `name set`; missing → `E_NOATTR` |
| `$11` | `stringLength` | 1 | bytes |
| `$12` | `substring` | 3 | `start len str`; clamps like Nix; negative `start` → `E_INDEX` |
| `$13` | `toString` | 1 | `STRING` as is; `INT` in decimal; `BOOL` → `"1"`/`""`; `NULL` → `""`; `LIST` → elements joined by spaces |
| `$14` | `concatStringsSep` | 2 | `sep list` |
| `$15` | `throw` | 1 | forces the message; aborts with `E_THROW`, the `STRING` pointer at `$0300` |
| `$16` | `abort` | 1 | as `throw`, `E_ABORT` |
| `$17` | `seq` | 2 | forces `a`, returns `b` |
| `$18` | `deepSeq` | 2 | deep-forces `a`, returns `b` |
| `$19` | `typeOf` | 1 | `"int"`, `"bool"`, `"null"`, `"string"`, `"list"`, `"set"`, `"lambda"` |
| `$1A` | `isInt` | 1 | |
| `$1B` | `isBool` | 1 | |
| `$1C` | `isString` | 1 | |
| `$1D` | `isNull` | 1 | |
| `$1E` | `isList` | 1 | |
| `$1F` | `isAttrs` | 1 | |
| `$20` | `isFunction` | 1 | |
| `$21` | `add` | 2 | `INT` only |
| `$22` | `sub` | 2 | |
| `$23` | `mul` | 2 | `ALU64` |
| `$24` | `div` | 2 | `ALU64`; `E_DIVZERO` |
| `$25` | `lessThan` | 2 | `INT` |
| `$26` | `bitAnd` | 2 | |
| `$27` | `bitOr` | 2 | |
| `$28` | `bitXor` | 2 | |
| `$29` | `listToAttrs` | 1 | `[ { name; value; } … ]`; first name wins, as in Nix |
| `$2A` | `intersectAttrs` | 2 | names from `a` that are in `b`, values from `b` |
| `$2B` | `removeAttrs` | 2 | `set [names]` |
| `$2C` | `partition` | 2 | `f list` → `{ right; wrong; }` |
| `$2D` | `baseNameOf` | 1 | last path component of a `STRING` |
| `$2E` | `dirOf` | 1 | everything before it; `"."` if none |
| `$2F` | `tryEval` | 1 | see "Catch frames" |

Arity is intrinsic to the id and lives in all three tables (`checks.nbc-opcodes`
diffs the primop table too).

### Deep force and the printer

Before the entry chunk's value is reported it is **deep-forced**: every `LIST`
element and `ATTRS` value, recursively, so that the harness sees plain data. That
is what tvix's test runner does, and why the `.exp` files contain no `<CODE>`.
The harness (`a2min --run-nbc`) then walks the object graph in the nursery and
prints it the way `nix-instantiate --eval --strict` would: `[ 1 2 ]`, `{ a = 1;
b = "x"; }` (names sorted, quoted when not identifiers), strings with Nix's
escapes (`\"`, `\\`, `\n`, `\r`, `\t`, `\${`), `<LAMBDA>` for functions.

Every gap in the numbering is deliberate room for the next slice (`$06`–`$0F`
constants, `$15` string ops, `$2D`–`$2F` more attrs/list ops, `$3A`–`$3F`, `$44`+).

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
| elements of a list literal | **lazy** (same rule as arguments): `[ (1 / 0) ]` is a fine list until element 0 is forced |
| values of an attribute set literal | **lazy**; names are `STRING` constants, sorted by bytes at compile time |
| parts of an interpolated string | strict — the string is needed whole |
| the set in `a.b`, `a ? b`, both sides of `//` and `++` | strict |
| `inherit x;` in a `let` | a pointer copy from the scope **outside** the `let` (`x = x` would be a self-reference); `inherit (e) x;` is `x = e.x`, lazy |

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

The entry chunk's `RETURN` value is deep-forced, and its **pointer** is written by
the VM to `$0300`:

```
$0300  value      4-byte pointer to the (deep-forced) result object, HEAP.md layout
$0320  status     $01 done; $FF error, with the error code at $0321
```

The harness reads the object graph straight out of the nursery. On `E_THROW` and
`E_ABORT`, `$0300` holds the message `STRING` instead.

Error codes: `E_BADOP $01`, `E_TYPE $02`, `E_DIVZERO $03`, `E_INFREC $04`,
`E_NOMEM $05` (nursery, value stack or call/force depth exhausted), `E_BADNBC $06`
(malformed image, or a slot read before it was set — a generator bug), `E_NOCARD
$07` (`MUL`/`DIV` with no card in any slot), `E_ASSERT $08`, `E_THROW $09`,
`E_ABORT $0A`, `E_NOATTR $0B` (selecting a missing attribute), `E_INDEX $0C`
(`head []`, `elemAt` out of range, negative `substring` start), `E_NOVAR $0D`
(an identifier no `with` set supplies). The harness (`a2min --run-nbc`) prints the value in
Nix syntax (`42`, `true`, `null`) so the scoreboard can compare it with the corpus's
`.exp` files, and prints `error: <name>` otherwise.

## Slice plan

1. **Integers, booleans, null; `let`; `if`; thunks with in-place update.** Done.
   Gate: `checks.scoreboard` passes every corpus test the generator can compile,
   and the count is in `tests/corpus/expected-pass.txt` so it can only go up.
2. **a. Lambdas, application, `assert`.** Done — `LAMBDA`, `CALL`, `ASSERT`.
   **b. Strings, lists, attribute sets, selection, 47 primops.** Done —
   `target/vm/data.s`, `equal.s`, `prims.s`. Selection on young sets is a
   CPU-side hash scan; `LOOKUP` serves promoted sets once slice 4 promotes.
   Two lessons the VM source records: any routine that runs bytecode (`FORCE`,
   `CALL`, equality, deep force) clobbers every scratch variable, so binary
   operations force both operands before reading either (`force_two`) and loops
   that call back into Nix keep their state on the CPU stack; and the VM binary
   is linker-capped at 12 KB because the first time it grew into the nursery the
   `true`/`false` singletons were silently overwritten.
3. **`tryEval` catch frames, `with`, formals, `rec`, attribute paths, legacy
   `let`, `__functor`, `TAIL_CALL`.** Done. A caught throw un-blackholes the
   thunks it unwound through (the VM keeps a stack of thunks under evaluation
   for this), so a failed thunk forced again throws again, as in Nix.
   `eval-okay-tail-call-1` (100 000 calls) no longer grows the CPU stack but
   still allocates an environment per call, so it waits for slice 4.
4. Builtins that touch the store, `import`, paths; the nursery collector runs
   during evaluation.
