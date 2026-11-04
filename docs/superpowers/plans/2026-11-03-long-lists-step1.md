# Long lists, step 1: every list primop to the nursery's size -- Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every primop that builds or reads a list works up to the largest list the nursery can hold (~16,000 elements), with the host's answers in every mode, where today `sort` gives out at ~493 elements, `filter` at ~971, `genList` at ~1,460 and a mature list past 5,119 cannot be read at all.

**Architecture:** Three changes, in the VM only (`target/vm/c`). Long mature lists are read through their header and one element at a time, never viewed whole. Builders that make new objects per element allocate their result first, nulled, and fill it a batch at a time, a reserve a batch. Primops that staged results on the value stack build into lists instead. Small lists keep today's paths, and the benchmarks are the gate.

**Tech Stack:** ORCA/C 65816 VM, a2min (Verilated card), `scripts/scoreboard.py`, `scripts/mutation-vm.sh`, `nix flake check`.

**Spec:** `docs/HEAP.md`, "Long lists (design, 2026-10-29)", Step 1 and Gates. Rules of work: `.claude/skills/vm-change/SKILL.md`. Work on `main` directly (owner, 2026-11-03).

## Global Constraints

- Counts are 16 bits; `obj_size` is 16 bits, so a list stays at or under 16,383 elements, and step 1's largest is the nursery's: (65,504 - 4) / 4 = 16,375. Past that stays `E_NOMEM` (step 2).
- `WINDOW_MAX` is 0x5000: a mature list of more than 5,119 elements cannot be viewed. A whole view of one stays `E_NOMEM`, loud. No reader may view it in its place.
- Value stack 1,024 slots, root stack 320: neither may hold a list's elements.
- No `obj_t *` C local across anything that collects (`force_*`, `do_call`, `call_with`, `nursery_reserve`, `store_field` into a mature object). Keep lists on `rstack`, re-read after.
- A list the collector can see has every slot valid: null or a value. `list_alloc` leaves slots uninitialised.
- A list's count never changes after allocation: it may not grow, nor shrink in place.
- A store into a list that may be mature goes through `store_field` (the barrier).
- Bank 0: no new statics, no new ORCA/C library routine (`~MOVE`, `~UMUL4`, `~SHIFTLEFT`, `~UDIV8`). Watch `omf2bin`'s bank-0 line: 16,403 today.
- Benchmarks (D29): `just bench` after every task. Slower fails the check; a slowdown is explained in the commit and recorded (`just bench-update`), and small lists take today's path.
- Valid Nix not implemented stays `E_UNSUP`; anything still too big stays `E_NOMEM`. Never a wrong answer.
- Fixture answers are the host's: `nix-instantiate --eval --strict f.nix > f.exp` (D7).
- Every new rule gets a mutation in `scripts/mutation-vm.sh`. Dry-check its pattern first: `sed "$expr" file | cmp -s - file && echo MISS`.

## Review Focus

1. **A long list as the whole answer**, typed at the REPL or returned by a program: `deep_force_top` views the list on every iteration and never turns the window ring over, though the spec says it does. A person expects the list printed (the printer reads over the bus and prints `<OOB>` only past 16,384). Pinned by Task 2's `eval-okay-long-list-result`.
2. **A throw inside a builder's callback, caught by `tryEval`**: `filter`'s or `sort`'s buffer is half filled. A person expects the next long list to work as if nothing happened. Pinned by Task 4's `tryEval` line, and Task 5's.
3. **A builder's batch boundary**: exactly one batch, one more, and the empty list. A person expects no difference. Pinned by Task 3's 128/129/0 lines.
4. **`sort` with a comparator that is not a strict order**, at length: Nix's answer depends on peeksort's exact comparisons. A person comparing with `nix` expects the same order. Pinned by Task 5's `a: b: true` line.
5. **A loop that views each mature element** without running bytecode in between: after ~3,700 elements the ring is full and it is `E_NOMEM` (`concatLists`' outer loop, `sort`'s force loop, `listToAttrs`' positions pass). Pinned by Task 2's 5,000 one-element sublists, and Task 5's 15,000-element sort.

## Non-goals (rulings in this plan, for the owner to overrule)

- **`fromJSON`'s arrays** stage their elements on the value stack (~1,000). A parser does not know an array's length before it reads it, so it would need a counting pass. That rides with long strings: a JSON text holding such an array is itself a long string.
- **List literals over ~1,000 elements** (`OP_LIST`: the bytecode pushes every element first). Fixing them is a compiler change, in both nbc and the on-target compiler.
- **`replaceStrings`' lists** (3n + 1 slots, ~339 pairs): a string primop. Long strings, later.
- **Sizes and offsets past 16 bits** (`obj_size`, `store_field`'s offset, the remembered set's): step 2's, for lists past 16,383 elements. Step 1's largest is 16,375.
- **Equality, comparison, `deepSeq`, `toJSON` of long mature lists** (`values_equal`, `cmp_lists`, json `tree`): step 2's audit of every `ELEMS` reader. Fixtures here never compare long lists.

---

### Task 1: Read a mature list through its header

**Files:**
- Modify: `target/vm/c/data.c` (`obj_hdr`, `force_pop_view`'s callers' mature path), `target/vm/c/run.c` (`force_top`, `force_slot`, `force_thunk`'s update, `call_enter`), `target/vm/c/vm.h` (`HDR_*`; the out-of-line path of `FORCE_TOP`/`FORCE_SLOT2`), `target/vm/c/prims.c` (`force_pop_list`, `length`, `head`, `elemAt`, `typeOf`, `is_tag`)
- Test: `tests/fixtures/eval-okay-long-list-readers.nix` + `.exp`

**Interfaces:**
- Produces:
  - `uint32_t obj_hdr(obj_t *o)` (data.c): the header word of any object without taking a view. Young, static or a window: read in place. Mature: from the window cache if the object is cached, else 4 bytes from the card (`card_read_at`). It never collects and never takes a ring slot.
  - `HDR_TAG(h)`, `HDR_AUX16(h)`, `HDR_AUX2(h)` (vm.h): fields of a header word, by bytes (`BYTE_OF`/`HI16`), with no shifts.
  - `obj_t *force_pop_list(uint16_t *n)` (prims.c): forces the top and pops it. It is `E_TYPE` unless the value is a list, as `need(T_LIST)` reports it. It sets `*n` to the count and returns the list's pointer as the stack held it, young or mature, never a view.
  - The forcing and type-check paths take no view of a mature LIST, ATTRS or STRING. These are the paths: `force_top`, `force_slot`, the out-of-line part of `FORCE_TOP`/`FORCE_SLOT2`, `force_thunk`'s update (`update_ptr`), `call_enter`, `typeOf`, `is_tag`. Each needs only the tag of those, so each reads `obj_hdr` first and views only a THUNK, INDIRECT or BLACKHOLE, which are small. The young path the dispatch loop takes inline is unchanged.

- [ ] **Step 1: Write the failing fixture.** `tests/fixtures/eval-okay-long-list-readers.nix`:

```nix
# A list past WINDOW_MAX (5,119 elements), mature, read through its header and
# an element at a time -- never viewed whole (HEAP.md "Long lists", step 1).
# churn fills the nursery after l is built, so l is promoted; l2 is a thunk
# updated with the mature list.
let
  l = builtins.concatLists (builtins.genList (i: builtins.genList (j: i * 100 + j) 100) 60);
  churn = builtins.foldl' (a: x: a + x) 0 (builtins.genList (x: x) 1000);
  l2 = builtins.seq churn l;
in builtins.seq l2 [
  (builtins.length l2) (builtins.head l2) (builtins.elemAt l2 5999)
  (builtins.typeOf l2) (builtins.isList l2) churn
]
```

Then `nix-instantiate --eval --strict` it into its `.exp`, and `git add` both.

- [ ] **Step 2: Watch it fail.** Run `nix develop -c just fixtures`. Expected: this fixture is `error: E_NOMEM`, from `mature_view`'s `WINDOW_MAX` test on `l2`. If it answers instead, `l2` was never mature or never long: grow `churn` until the RED run is `E_NOMEM`.

- [ ] **Step 3: Implement `obj_hdr`, `HDR_*`, `force_pop_list` and the header-first mature path** in each forcing and type-check path above. Then `length`, `head` and `elemAt` use `force_pop_list` and `list_elem`.

- [ ] **Step 4: Watch it pass, in every mode.** Run `just fixtures`, `just fixtures-gc`, `just fixtures-gc-period 2`, `just fixtures-gc-period 3`, `just fixtures-reserve`, `just fixtures-rset`, `just fixtures-mature` and `A2MIN_VERB_MASK=4`. Expected: `0 fail, 0 error` in each. `A2MIN_NO_CARD=1` may make this fixture `E_NOMEM`, with the rest as before.

- [ ] **Step 5: Mutations.** In `scripts/mutation-vm.sh`, add this fixture to `FIXTURES`; it runs on 512 KB halves, where it fits (~100 KB live). Add these mutations:
  - "force_top views a mature list": `force_top`'s header read removed, so the view comes back. Caught as `E_NOMEM`.
  - "a mature thunk is taken for a value": the header path skips the view for a THUNK too. Caught as wrong answers.
  - "length views its list": `length` back on `force_pop_as`. Caught as `E_NOMEM`.

  Then `just mutation-vm`. Expected: `MUTATION PASS`, with the count 3 higher than before.

- [ ] **Step 6: Bench and bank 0.** Run `just bench`. The forcing paths' mature branch is the only change on a hot path, so expect the same to the noise. Explain any slowdown and `just bench-update`. Bank 0 stays 16,403.

- [ ] **Step 7: Docs, the flake check, commit.** In HEAP.md's Step 1, record "readers through the header: done", with what was measured. Then `git add -A && nix flake check -L --max-jobs 2`. Expected: `all checks passed!`. Commit "long lists 1/8: a mature list read through its header -- force, typeOf, length, head, elemAt", following the vm-change skill's message rules, and push.

### Task 2: Copy and walk long mature lists

**Files:**
- Modify: `target/vm/c/data.c` (`list_copy`, `deep_force_top`), `target/vm/c/prims.c` (`tail`, `concatLists`, and every other `force_pop_as(T_LIST, ...)` caller that only needs the count: `map`'s and `filter`'s inputs, `foldl'`, `elem`, `any`/`all`, `catAttrs`, `removeAttrs`, `genericClosure`, `listToAttrs`, `groupBy`, `zipAttrsWith`, `partition`, `sort`), `target/vm/c/run.c` (`OP_CONCAT`), `target/vm/c/strings.c` (`coerce_top`, `concatStringsSep`, path interpolation, `findFile`, `appendContext`, `replaceStrings`' counts), `target/vm/c/json.c` and `target/vm/c/derivation.c` (count-only views of lists)
- Test: `tests/fixtures/eval-okay-long-list-copies.nix`, `tests/fixtures/eval-okay-long-list-result.nix` (+ `.exp`)

**Interfaces:**
- Consumes: `obj_hdr`, `HDR_*` and `force_pop_list` (Task 1).
- Produces: `void list_copy(obj_t *dst, uint16_t at, obj_t *src, uint16_t from, uint16_t k)` (data.c). It copies `k` elements of `src`, young or mature and of any length, from index `from` into `dst` at `at`. A young `src` is a `memcpy`; a mature one is read in `card_read_at`/`card_read_more` bursts. `dst` must be a fresh young list, so no barrier is needed. It is called after the caller's reserve and never collects.

- [ ] **Step 1: Write the failing fixtures.**

`eval-okay-long-list-copies.nix` has the same `l`, `churn` and `l2` as Task 1, plus:
```nix
  s = builtins.concatLists (builtins.genList (i: builtins.genList (j: [ (i * 100 + j) ]) 100) 50);
in builtins.seq l2 [
  (builtins.length (builtins.tail l2)) (builtins.elemAt (builtins.tail l2) 5998)
  (builtins.length (l2 ++ [ 1 ])) (builtins.elemAt ([ 1 ] ++ l2) 6000)
  (builtins.length (builtins.concatLists [ l2 l2 ]))
  (builtins.length (builtins.concatLists s))
  (builtins.foldl' (a: x: a + x) 0 l2) (builtins.elem 5999 l2)
  (builtins.any (x: x == 5999) l2) (builtins.all (x: x < 6000) l2)
  (builtins.length (builtins.filter (x: x < 10) l2))
  (builtins.toString (builtins.filter (x: x < 3) l2))
]
```
Here `s` is 5,000 mature one-element sublists: `concatLists`' outer loop views each of them, which fills the ring unless it turns the ring over between elements.

`eval-okay-long-list-result.nix` keeps the same `l2`, but its answer is the list itself: `builtins.seq l2 l2`. This is Review Focus 1, `deep_force_top` and the printer on 6,000 elements.

- [ ] **Step 2: Watch them fail.** Run `just fixtures`. Expected: both `E_NOMEM`.

- [ ] **Step 3: Implement.**
  - `list_copy`.
  - `tail`, `OP_CONCAT` and `concatLists`' sublists copy with `list_copy` and read counts with `obj_hdr`; `view_two` is no longer used for `++`.
  - Every count-only `force_pop_as(T_LIST)` and `AUX16(deref(...))` of a list listed above becomes `force_pop_list` or `HDR_AUX16(obj_hdr(...))`.
  - `windows_maybe()` goes between elements, where no view is held, in `concatLists`' two loops and in `deep_force_top`.
  - `deep_force_top` reads the container's tag, count and mark with `obj_hdr`, and a list's elements with `list_elem`. A set keeps today's view: sets past `WINDOW_MAX` are the later item. Its mark write stays `store_byte`.

- [ ] **Step 4: Every mode**, as in Task 1 Step 4. Expected: `0 fail, 0 error`.

- [ ] **Step 5: Mutations.** Add both fixtures to `FIXTURES`. Add these mutations:
  - "tail views a mature list": `tail` back on `force_pop_as` and `memcpy`.
  - "concatLists' outer loop never turns the ring": its `windows_maybe` removed.
  - "deep_force_top never turns the ring": its `windows_maybe` removed.
  - "++ copies through a view": `view_two` again.

  Then `just mutation-vm`. Expected: `MUTATION PASS`.

- [ ] **Step 6: Bench, bank 0, docs, flake check, commit, push**, as in Task 1 Steps 6-7. HEAP.md also says that `deep_force_top` now turns the ring over, which the design had assumed it already did. Commit: "long lists 2/8: long mature lists copied and walked an element at a time -- tail, ++, concatLists, deepSeq of the answer".

### Task 3: `genList` and `map` in batches

**Files:**
- Modify: `target/vm/c/vm.h` (`LIST_BATCH`), `target/vm/c/data.c` (`list_alloc_null`, `list_put`), `target/vm/c/prims.c` (`prim_genList`, `prim_map`), `scripts/scoreboard.py` (`--skip-file`), `flake.nix`, `justfile`
- Create: `tests/corpus/skip-gc-stress.txt`, `tests/corpus/skip-mature-stress.txt`
- Test: `tests/fixtures/eval-okay-long-genlist-map.nix`, `tests/fixtures/eval-okay-long-genlist-map-small.nix` (+ `.exp`)

**Interfaces:**
- Consumes: `list_copy`, `force_pop_list` (Tasks 1-2).
- Produces:
  - `#define LIST_BATCH 128` (vm.h): the elements a builder makes per reserve. It is at most `RSET_N / 2`, so one batch's stores into a promoted list cannot fill the remembered set.
  - `obj_t *list_alloc_null(uint16_t n)` (data.c): `list_alloc` with every slot null. The caller has reserved `4n + 4`.
  - `void list_put(uint16_t r, uint16_t i, obj_t *v)` (data.c): `store_field(rstack[r], 4 + 4i, v)`, i.e. element `i` of the list in root-stack slot `r`, through the barrier.
  - `scoreboard.py --skip-file FILE` (append): one test name per line, `#` comments; a name that matches no test is an error, as with `--skip`.
- The batched path:
  1. Reserve `4n + 4`, allocate the list nulled, and root it.
  2. For each batch of up to `LIST_BATCH` elements: reserve `batch * per`, then make the batch's elements and `list_put` each one.
  3. When `n <= LIST_BATCH`, take today's path unchanged: one reserve, `list_alloc`, a plain fill. That keeps small lists exactly as fast.

- [ ] **Step 1: Write the failing fixtures.** `eval-okay-long-genlist-map.nix`:

```nix
# genList and map past their old ceilings (~1,460 and ~1,995: one reservation
# for the whole result) to 15,000, built a batch at a time (HEAP.md "Long
# lists", step 1); and one batch (128), one more, and none.
let
  l = builtins.genList (x: x * 3) 15000;
  m = builtins.map (x: x + 1) l;
in [
  (builtins.length l) (builtins.elemAt l 14999) (builtins.length m)
  (builtins.elemAt m 0) (builtins.elemAt m 14999) (builtins.foldl' (a: x: a + x) 0 m)
  (builtins.length (builtins.genList (x: x) 128)) (builtins.elemAt (builtins.genList (x: x) 129) 128)
  (builtins.genList (x: x) 0) (builtins.map (x: x) [ ])
]
```

`-small` is the same at 2,500, which is past both old ceilings, and smaller if the mature-stress measurement in Step 4 needs it.

- [ ] **Step 2: Watch them fail.** Expected: both `E_NOMEM`, from `nursery_reserve_n` in `genList`.

- [ ] **Step 3: Implement** `LIST_BATCH`, `list_alloc_null`, `list_put`, the batched paths of `genList` and `map`, and `--skip-file`. `flake.nix` and the `justfile` use the two skip files:
  - the GC-stress and period passes use `skip-gc-stress.txt`;
  - the mature-stress pass uses `skip-mature-stress.txt`, where the existing `--skip fixtures/eval-okay-card-heap-big` moves.

- [ ] **Step 4: Every mode, and the sizes.** Measure each new fixture in each mode: cycles, and wall time against the scoreboard's 600 s. A full-size fixture past the budget in a stress mode goes in that mode's skip file, with a `#` line giving the measurement. Each family must keep a fixture past its old ceiling in every mode, so size the `-small` variant to fit mature stress. Expected: `0 fail, 0 error` everywhere, and a ledger line for every skip.

- [ ] **Step 5: Mutations.** Add `-small` to `FIXTURES`; the full size does not fit a 512 KB half, at ~660 KB live. Add these mutations:
  - "a batched list's slots not nulled": `list_alloc_null` becomes `list_alloc`. Caught under GC stress.
  - "a store into the promoted list without the barrier": `list_put` writes the card directly. Caught.
  - "the batched path never taken": the threshold set to 65,535. Caught as `E_NOMEM`.

  Then `just mutation-vm`.

- [ ] **Step 6: Bench, bank 0, docs, flake check, commit, push.** `genList` and `map` are in the bench programs, so an unchanged small path must show no change. Commit: "long lists 3/8: genList and map in batches, to 15,000".

### Task 4: `filter`, `partition`, `concatMap` and `catAttrs` into lists, not the value stack

**Files:**
- Modify: `target/vm/c/prims.c` (`prim_filter`, `prim_partition`, `prim_concatMap`, `prim_catAttrs`; `concatLists`' core, factored so that `concatMap` can call it on a list in a root-stack slot)
- Test: `tests/fixtures/eval-okay-long-filter-partition.nix`, `-small` (+ `.exp`)

**Interfaces:**
- Consumes: `list_alloc_null`, `list_put`, `list_copy`, `force_pop_list`.
- Produces: `void concat_lists_at(uint16_t r)` (prims.c): pushes the concatenation of the list of lists in `rstack[r]`. This is `concatLists`' body after its argument is forced.
- The rule:
  1. A buffer list at the input's length, nulled and rooted. Each kept element (or mapped list) is `list_put` into it.
  2. Then a fresh list of the exact length: reserve `4k + 4`, which may promote the buffer, then `list_copy`. If `k == n`, the buffer is the answer itself.
  3. Below `LIST_BATCH` elements of input, keep today's value-stack path.

- [ ] **Step 1: Write the failing fixture.**

```nix
# filter, partition, concatMap and catAttrs past their old ceilings (~971,
# ~493: results staged on the value stack), into lists (HEAP.md "Long lists",
# step 1). A throw inside filter's predicate, caught, leaves a half-filled
# buffer behind; the lists after it must not mind.
let
  l = builtins.genList (x: x) 15000;
  f = builtins.filter (x: x / 2 * 2 == x) l;
  p = builtins.partition (x: x < 9000) l;
  c = builtins.concatMap (x: [ x x ]) (builtins.genList (x: x) 7000);
  a = builtins.catAttrs "v" (builtins.genList (x: { v = x; }) 12000);
  t = builtins.tryEval (builtins.filter (x: if x == 3000 then throw "half" else true) l);
in [
  (builtins.length f) (builtins.elemAt f 7499) (builtins.length p.right)
  (builtins.length p.wrong) (builtins.elemAt p.wrong 0) (builtins.length c)
  (builtins.elemAt c 13999) (builtins.length a) (builtins.elemAt a 11999)
  t.success (builtins.length (builtins.filter (x: true) l))
]
```

`-small` is the same at 2,000 and 1,000.

- [ ] **Steps 2-6: as in Task 3.** Expected RED: `E_NOMEM`, the value stack being full. Mutations:
  - "filter's answer keeps the buffer's length": the exact copy skipped. Caught as a wrong `length`.
  - "partition stores without the barrier".
  - "concatMap stages on the value stack again".

  Commit: "long lists 4/8: filter, partition, concatMap, catAttrs into lists, to 15,000".

### Task 5: `sort` with a list for its buffer

**Files:**
- Modify: `target/vm/c/prims.c` (`prim_sort`, `sort_less`, `sort_swap`, `sort_peek` and its helpers)
- Test: `tests/fixtures/eval-okay-long-sort.nix`, `-small` (+ `.exp`)

**Interfaces:**
- Consumes: `list_alloc_null`, `list_put`, `force_pop_list`.
- Peeksort, and the comparisons it makes in the order it makes them, stay exactly as they are; only their storage moves.
  - The algorithm addresses `2n` virtual slots through `obj_t *sort_get(uint16_t i)` and `void sort_set(uint16_t i, obj_t *v)`.
  - Slots `[0, n)` are the data list and `[n, 2n)` the working buffer, two lists in root-stack slots (`list_elem`, `list_put`).
  - When `2n + 8` fits the value stack (today's test), the same accessors use value-stack slots, which is today's storage.
  - The answer is the data list itself, exactly `n` long. Its force loop calls `windows_maybe()` between elements.

- [ ] **Step 1: Write the failing fixture.**

```nix
# sort past ~493 (2n value-stack slots) to 15,000, its buffer a list, and the
# comparisons peeksort makes unchanged: a pseudo-random order, so the buffer is
# used; a stable sort of 2,000 records by a repeated key; a comparator that is
# not an order; a throw from the comparator, caught (HEAP.md "Long lists").
let
  r = builtins.genList (i: (i * 7919 + 13) - (i * 7919 + 13) / 15013 * 15013) 15000;
  s = builtins.sort builtins.lessThan r;
  k = builtins.sort (a: b: a.k < b.k)
        (builtins.genList (i: { k = i - i / 10 * 10; inherit i; }) 2000);
  w = builtins.sort (a: b: true) (builtins.genList (x: x) 2000);
  t = builtins.tryEval (builtins.sort (a: b: if a == 777 then throw "x" else a < b) r);
in [
  (builtins.length s) (builtins.head s) (builtins.elemAt s 14999) (builtins.elemAt s 7500)
  (builtins.elemAt k 0).i (builtins.elemAt k 1).i (builtins.elemAt k 1999).i
  (builtins.elemAt w 0) (builtins.elemAt w 1) (builtins.elemAt w 1999) t.success
]
```

`-small` is the same at 2,000.

- [ ] **Steps 2-6: as in Task 3.** Expected RED: `E_NOMEM` from `prim_sort`'s value-stack check.
  - Check also that `eval-okay-sort` and `eval-okay-sort-noorder` are unchanged: they pin peeksort's comparisons.
  - Mutations: "sort's list path writes without the barrier"; "sort's force loop never turns the ring". The small path, today's storage, is pinned by `eval-okay-sort` and `eval-okay-sort-noorder` and by `just bench`.
  - Commit: "long lists 5/8: sort's buffer a list, to 15,000; peeksort's comparisons unchanged".

### Task 6: `listToAttrs`, `groupBy`, `zipAttrsWith`, `genericClosure` without value-stack staging

**Files:**
- Modify: `target/vm/c/prims.c` (`prim_listToAttrs` and its positions pass, `prim_groupBy`, `prim_zipAttrsWith`, `prim_genericClosure`)
- Test: `tests/fixtures/eval-okay-long-sets.nix`, `-small` (+ `.exp`)

**Interfaces:**
- Consumes: `list_alloc_null`, `list_put`, `list_copy`, `force_pop_list`.
- What each stages today moves into parallel lists in root-stack slots:
  - `listToAttrs` and `groupBy`: names and values;
  - `zipAttrsWith`: sets and names;
  - `genericClosure`: work items and keys.

  `zipAttrsWith`'s single 16-bit reserve (`total * 84 + 8`) becomes one reserve per name, as a batch. Each result set stays within the set's own ceiling (~1,460 attributes once mature), as the spec says.
  - `listToAttrs`' positions pass calls `windows_maybe()` between elements.

- [ ] **Step 1: Write the failing fixture.**

```nix
# listToAttrs, groupBy, zipAttrsWith, genericClosure past ~500 (two value-stack
# slots an element), each to its set's own ceiling (HEAP.md "Long lists").
let
  n = builtins.genList (i: { name = "a${toString i}"; value = i; }) 1400;
  d = builtins.genList (i: { name = "b${toString (i - i / 1000 * 1000)}"; value = i; }) 3000;
  g = builtins.groupBy (x: toString (x - x / 7 * 7)) (builtins.genList (x: x) 6000);
  z = builtins.zipAttrsWith (k: vs: builtins.length vs) (builtins.genList (i: { a = i; b = i; }) 3000);
  c = builtins.genericClosure {
    startSet = [ { key = 0; } ];
    operator = x: if x.key < 1999 then [ { key = x.key + 1; } ] else [ ];
  };
in [
  (builtins.length (builtins.attrNames (builtins.listToAttrs n))) (builtins.listToAttrs n).a1399
  (builtins.length (builtins.attrNames (builtins.listToAttrs d))) (builtins.listToAttrs d).b999
  (builtins.length g."0") (builtins.length g."6") z (builtins.length c)
]
```

`-small` is the same at 700, 1,000, 1,000, 1,000 and 700: past ~500, for the stress modes.

- [ ] **Steps 2-6: as in Task 3.** Expected RED: `E_NOMEM`.
  - Measure `genericClosure` first: its duplicate check may be quadratic. If 2,000 takes more than ~10 s plain, use the largest size that does, past 500, and ledger it.
  - Mutations: "listToAttrs stages on the value stack again"; "groupBy's values stored without the barrier"; "listToAttrs' positions pass never turns the ring".
  - Commit: "long lists 6/8: listToAttrs, groupBy, zipAttrsWith, genericClosure off the value stack".

### Task 7: `concatStringsSep` and `toString` of long lists

**Files:**
- Modify: `target/vm/c/strings.c` (`concatStringsSep`'s context and coerce paths, `coerce_top`'s list case)
- Test: `tests/fixtures/eval-okay-long-strings-of-lists.nix`, `-small` (+ `.exp`)

**Interfaces:**
- Consumes: `list_alloc_null`, `list_put`, `force_pop_list`.
- The pieces staged on the value stack, one or two an element, go into a list in a root-stack slot. The result string keeps its 64 KB ceiling, so the fixture stays under it.

- [ ] **Step 1: Write the failing fixture.**

```nix
# concatStringsSep where each element has context or must be coerced, and
# toString of a list: past ~1,000 (a value-stack slot a piece) to 15,000
# elements, the strings under 64 KB (HEAP.md "Long lists", step 1).
let
  d = derivation { name = "d"; system = "x"; builder = "/b"; };
  c = builtins.concatStringsSep "," (builtins.genList (i: builtins.substring 0 1 "${d}") 15000);
  o = builtins.concatStringsSep "," (builtins.genList (i: { __toString = s: "x"; }) 15000);
  t = toString (builtins.genList (i: i - i / 10 * 10) 15000);
in [
  (builtins.stringLength c) (builtins.hasContext c) (builtins.stringLength o)
  (builtins.substring 0 9 o) (builtins.stringLength t) (builtins.substring 29990 9 t)
]
```

`-small` is the same at 2,000.

- [ ] **Steps 2-6: as in Task 3.** Expected RED: `E_NOMEM`.
  - Mutations: "concatStringsSep's context path stages on the value stack again"; "toString of a list stages on the value stack again".
  - Commit: "long lists 7/8: concatStringsSep and toString of long lists".

### Task 8: The ceilings measured again, and the documents

**Files:**
- Modify: `docs/HEAP.md` (Step 1: done; the "Works to about" table measured again), `docs/STATUS.md` (the roadmap's step 1 struck, the scoreboard number, bank 0), `docs/VM.md` (its ceilings)
- Test: the measurement itself

- [ ] **Step 1: Measure every primop's ceiling again** on `a2min --run-nix`, with the method the design used for the table: each fed a list built by `concatLists`, doubling until it fails, then bisecting. Record the new "Works to about" column next to the old one. Expected: every list primop reaches ~16,000 (the nursery's 16,375 less what its own work keeps live), and the sets and strings reach their own ceilings.
- [ ] **Step 2: The documents**, with the measurements: HEAP.md Step 1 marked done; STATUS.md's roadmap struck through, and its scoreboard and bank-0 lines; VM.md's ceilings paragraph.
- [ ] **Step 3: Full flake check, commit, push.** `nix flake check -L --max-jobs 2`. Expected: `all checks passed!`. Commit: "long lists 8/8: every list primop to the nursery's size, measured".
