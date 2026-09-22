# tvix evaluation corpus (vendored)

This is the **scoreboard** — the headline metric of the project is how much of it
the 65816 VM passes.

## Provenance

Copied verbatim from the tvix repository, `eval/src/tests/`:

- upstream: <https://github.com/tvlfyi/tvix> (mirror of `code.tvl.fyi/depot`)
- commit: `9fbca101331eecbf5c1cd44861f3d24aff101b82` (2026-09-01)
- licence: **GPL-3.0** — see [LICENSE](LICENSE). These files are **test data only**;
  nothing in this repository links against or derives code from them, and no
  `.nbc` compiled from them is shipped anywhere.

`nix_tests/` is tvix's port of upstream Nix's own language tests (originally
LGPL-2.1, redistributed by tvix under its GPL-3.0 terms); `tvix_tests/` are tvix's
own additions.

## What was vendored

- `eval-okay-*.nix` with their `eval-okay-*.exp` expected values — 370 tests,
  including 24 under `notyetpassing/`, which tvix itself does not pass yet and
  which the scoreboard reports separately, keeping tvix's convention.
- The support files some tests `import` (`lib.nix`, `imported*.nix`, `dir1/`…,
  `readDir/`, `data`, `binary-data`, `directory/`), so the layout is unchanged
  and relative paths in the tests still resolve.

**Not** vendored: `eval-fail-*` and `parse-fail-*` (error tests) and the `.rs`
harness files. The failure corpus arrives when the VM reports errors well enough
to be scored on them.

## How it is scored

`scripts/scoreboard.py` compiles each test with `tools/nbc` and runs it on the
65816 VM (`target/vm/`) under `a2min --run-nbc`, then compares the printed value
with the `.exp` file. Every test lands in exactly one bucket:

| Bucket | Meaning |
|---|---|
| `pass` | value identical to `.exp` |
| `fail` | the VM ran and produced something else — a real bug |
| `error` | the VM aborted (`E_*`) |
| `unsupported` | the generator refused the source (outside the current slice) — honest, not a VM failure |
| `notyetpassing` | tvix does not pass it either |

`tests/corpus/expected-pass.txt` lists every test currently passing.
`checks.scoreboard` fails if any of them stops passing, so the number can only
go up, and updating the file is how a new slice claims its progress.

Do not edit the vendored files. To refresh, re-copy from a newer tvix commit and
update this README.
