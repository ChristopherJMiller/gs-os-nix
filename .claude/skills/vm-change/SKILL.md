---
name: vm-change
description: How to make and verify a change to the gs-os-nix 65816 VM (target/vm/c), the generator (tools/nbc) or a Nix builtin -- getting the host's answer, the bank-0 and GC rules, the fixtures, mutation tests and gates, run one at a time. Use for any new primop, Nix semantics, derivation or string-context work, or VM optimisation in this repo.
---

# Changing the VM, the gs-os-nix way

The project's rules (docs/STATUS.md "Resuming", DECISIONS.md D7/D14/D19) in
working order. Every step below exists because skipping it once produced a
plausible wrong answer or a lost hour.

## 0. Environment

- Everything runs in the dev shell: `nix develop -c just <recipe>`. `python3`
  exists only there.
- The login shell is **zsh, which does not word-split**: `env $e cmd` with
  `e="A=1 B=2"` sets one variable. Write multi-flag loops as bash scripts and
  run them with `nix develop -c bash script.sh`.
- Long jobs (mutation runs, `nix flake check`, GC-stress sweeps) run **one at a
  time, in the background**. Parallel ones were killed by the low-memory reaper.

## 1. The host's answer first (D7, D19)

The fixtures are the host's own output, never a model's.

- A value: `nix-instantiate --eval --strict f.nix > f.exp`.
- A derivation's ATerm: `nix-instantiate f.nix` prints the `.drv` path; `cat`
  it. A fixture with a `drvPath` in a string needs `--read-write-mode` with
  `--eval --strict`: Nix computes that closure from the store. If Nix cannot write the `.drv` (e.g. an unescaped quote in `system`),
  make a variant it can write and diff that.
- Before writing C for anything subtle, check your understanding with a
  throwaway Python model in the scratchpad against the host -- this found that
  `__contentAddressed = false` leaves the environment and how modulo hashes of
  non-leaf inputs work. The model never becomes a fixture or a script in the
  repo: D7, and `checks.fixture-purity` greps the generator for hashing code.
- Write the design into the docs (DERIVATION.md, HEAP.md, BYTECODE.md) before
  the code, including what was measured on the host.

## 2. The VM's rules (target/vm/c)

- **Bank 0 is nearly full.** Every static, and every ORCA/C library routine the
  code links, lands in bank 0 whatever its segment; string literals stay with
  their code. Put new code in a `segment "prims"` file (bank 4): `prims.c`
  (helpers, lists, sets), `strings.c` (strings, context), `derivation.c`,
  `sha256.c`; what they share goes in `prims.h`. A new file goes in
  `target/vm/c/SOURCES`, the one list every build reads. `just bank-gate` holds bank 0 to the exact size in
  `target/vm/bank0.txt` and to an allowlist of runtime routines: when it
  changes, find out why in `target/build/cvm.map`, say so in the commit, and
  `just bank-gate --update`. ORCA/C's lint is on (`vm.h`): an unused variable
  or an unprototyped call is a build error. A struct copy links
  `~MOVE`, a variable left shift `~SHIFTLEFT`, 64-bit `/` `%` are `~UDIV8`
  (~4,000 cycles); 32-bit shifts are ~300-cycle calls -- use `BYTE_OF`/`HI16`.
- **Anything that runs bytecode may collect and move young objects.** No
  `obj_t *` C local across `force_*`, `do_call`, `coerce_top`,
  `values_equal`: keep it on `rstack`/`vstack`. Allocation never collects;
  an operation that allocates in proportion to its input calls
  `nursery_reserve` first, with everything live on a stack, and re-views
  afterwards.
- **A view (`deref`) of a mature object is valid until the window ring turns
  over** -- at a bytecode boundary, or at a `windows_maybe()` you call where
  you hold no view.
- **Strings carry context** (`CTX(s)`): any new operation that builds a string
  from strings must unite their contexts (`ctx_join`) or a dependent
  derivation silently loses an input.
- Valid Nix not implemented yet is `E_UNSUP`, never a guess.
- **The C stack has a 1 KB margin above `$0400`** (VM.md "The C stack"):
  every path between two `stack_check()`s must fit it. A new path deeper than
  ~700 bytes -- a big local array, a call into `float.c` or `toml.c`, which
  have no checks of their own -- calls `stack_need(n)` first. Frame sizes are
  readable from each prologue (`TSC SEC SBC #n TCS`) in `cvm.bin`; `just
  stack-stress` runs the deepest programs at every stack depth, and a2min
  ends any run that goes under the floor.

## 3. Tests that can fail (D14)

- Fixtures in `tests/fixtures/`: `eval-okay-*.nix` + `.exp` (host value),
  `eval-fail-*` (Nix rejects it too; `.exp` is the VM's error line, e.g.
  `error: E_TYPE`), `refuse-*` (valid Nix the VM declines; `error: E_UNSUP`).
  Design a fixture so each rule changes exactly one visible answer.
- `just fixtures`, then every mode -- GC stress (`--gc-stress`),
  `A2MIN_GC_PERIOD=2` and `=3` with `--gc-stress`, `A2MIN_RESERVE_STRESS=1`,
  `A2MIN_RSET_LIMIT=1`, `A2MIN_VERB_MASK=4` (SHA-256 on the CPU),
  `A2MIN_NO_CARD=1`, each through `scripts/scoreboard.py --corpus
  tests/fixtures`. (`eval-okay-big-operations` is `E_NOMEM` with no card by
  design.)
- Add a mutation per rule to `scripts/mutation-vm.sh` (sed on a copy;
  a pattern that misses fails the run). Dry-check each pattern first:
  `sed "$expr" file | cmp -s - file && echo MISS`. Then `just mutation-vm`.
- Generator changes: `just nbc-test`.

## 4. Gates, in order, one at a time

1. `just scoreboard`, then `just scoreboard-update` to claim new passes (the
   ratchet is `tests/corpus/expected-pass.txt`), `just bank-gate`, and `just
   c-lint` (seconds: host clang `-Werror` and gcsafe, which flags an `obj_t *`
   or a view read after a call that may collect; mark a deliberate exception
   `/* gcsafe: <why> */` on the line it names). After a new opcode, primop
   or error code, `just opcodes` (seconds): one row per primop in BYTECODE.md,
   its name in a2min's table -- the flake's `nbc-opcodes` fails otherwise,
   after an hour of other checks.
2. `just bench` (seconds): the six speed benchmarks against
   `tests/bench/cycles.txt` -- slower fails, faster asks for `just
   bench-update` (D29). `just stack-stress` (minutes) if a deep path, a big
   frame or `STACK_MIN` changed. Before claiming a collector change safe, the
   corpus too under GC stress (`--gc-stress` with `--corpus tests/corpus/tvix`,
   periods 1-3): no check runs it, and it found the `LOOKUP` mark bug; only
   `eval-okay-tail-call-1` fails it (E_NOMEM, the card's half-space).
3. `just verb-economics` if hashing or a card verb changed (needs `just
   fixtures` first so the workload is compiled); add the fixture to
   `VERB_WORKLOAD` in both the justfile and flake.nix if it exercises a verb.
4. **`git add` every new file**, then `nix flake check -L --max-jobs 2` in the
   background (~12 min). The flake sees only tracked files: an untracked
   fixture is silently not tested. The "ABC: Error: Abc_FrameUpdateGia()" line
   in rtl-synth is old and harmless.

## 5. Docs and the commit

- Keep STATUS.md (the layer row, the roadmap, bank 0, the scoreboard number)
  and the affected design docs current in the same commit.
- Commit message: what changed and how, what was MEASURED (cycles, bus
  accesses, bank-0 bytes, scoreboard), and WHAT WAS WRONG on the way. Use the
  repo's configured git identity (do not pass `-c user.email`).
