#!/usr/bin/env python3
"""The scoreboard: how much of the tvix evaluation corpus does the 65816 VM pass?

For every eval-okay-*.nix under each --corpus root:
  1. tools/nbc compiles it            -> rc 2 means UNSUPPORTED (outside the slice)
  2. a2min --run-nbc runs it on the VM -> prints `result: <value>` or `error: <E_*>`
  3. the printed value is compared with the .exp file

Buckets: pass, fail (ran, wrong value), error (VM aborted), unsupported,
notyetpassing (tvix itself does not pass these; scored separately).

--expected FILE is the ratchet: every test listed must still pass or the run
fails; newly passing tests are reported and, with --update, written back. This is
what makes the headline number only able to go up.

Compiled fixtures are left in --build next to a copy of their source so that
scripts/fixture-purity.sh can check each one against what it was compiled from.
"""
import argparse
import os
import shutil
import subprocess
import sys


def run(cmd, timeout=120):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return -1, "", "timeout"


def find_tests(root):
    out = []
    for dirpath, _, files in os.walk(root):
        for f in sorted(files):
            if f.startswith("eval-okay-") and f.endswith(".nix"):
                path = os.path.join(dirpath, f)
                exp = path[:-4] + ".exp"
                if not os.path.exists(exp):
                    continue
                rel = os.path.relpath(path, root)[:-4]
                nyp = "notyetpassing" in rel.split(os.sep)
                out.append((rel, path, exp, nyp))
    return sorted(out)


def vm_result(stdout):
    """The harness prints exactly one of these lines."""
    for line in stdout.splitlines():
        if line.startswith("result: "):
            return "value", line[len("result: "):].strip()
        if line.startswith("error: "):
            return "error", line[len("error: "):].strip()
    return "error", "no result line"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nbc", required=True, help="the fixture generator binary")
    ap.add_argument("--a2min", required=True)
    ap.add_argument("--vm", required=True, help="target/build/cvm.bin (docs/VM.md)")
    ap.add_argument("--corpus", action="append", required=True)
    ap.add_argument("--build", required=True, help="where compiled fixtures go")
    ap.add_argument("--expected", required=True, help="the ratchet file")
    ap.add_argument("--update", action="store_true", help="write new passes into --expected")
    ap.add_argument("--verbose", "-v", action="store_true")
    ap.add_argument("--gc-stress", action="store_true",
                    help="collect the nursery at every bytecode boundary (slow; a collector test)")
    args = ap.parse_args()
    run_mode = "--run-nbc-gc-stress" if args.gc_stress else "--run-nbc"

    os.makedirs(args.build, exist_ok=True)
    expected = set()
    if os.path.exists(args.expected):
        with open(args.expected) as f:
            expected = {l.strip() for l in f if l.strip() and not l.startswith("#")}

    buckets = {k: [] for k in ("pass", "fail", "error", "unsupported", "notyetpassing")}
    details = {}

    for root in args.corpus:
        if not os.path.isdir(root):
            continue
        tag = os.path.basename(root.rstrip("/"))
        for rel, nix, exp, nyp in find_tests(root):
            name = f"{tag}/{rel}"
            with open(exp) as f:
                want = f.read().strip()

            # Keep source and fixture side by side for the purity check.
            base = os.path.join(args.build, name.replace(os.sep, "__"))
            shutil.copyfile(nix, base + ".nix")
            rc, _, err = run([args.nbc, nix, "-o", base + ".nbc"])
            if rc == 2:
                bucket, got = "unsupported", err.strip().splitlines()[-1] if err.strip() else ""
            elif rc != 0:
                bucket, got = "error", f"generator rc={rc}: {err.strip()}"
            else:
                rc, out, err = run([args.a2min, run_mode, args.vm, base + ".nbc"], timeout=600)
                kind, got = vm_result(out)
                if kind == "error":
                    bucket = "error"
                elif got == want:
                    bucket = "pass"
                else:
                    bucket = "fail"
            if nyp and bucket != "pass":
                bucket = "notyetpassing"
            buckets[bucket].append(name)
            details[name] = (bucket, want, got)
            if args.verbose or bucket == "fail":
                print(f"  {bucket:<13} {name}" + ("" if bucket == "pass" else f"   want {want!r} got {got!r}"))

    total = sum(len(v) for v in buckets.values())
    passing = set(buckets["pass"])
    # The ratchet only judges tests this run looked at (a fixtures-only run
    # must not report the whole corpus as regressed).
    seen = set(details)
    regressions = sorted((expected & seen) - passing)
    new = sorted(passing - expected)

    # Why is the rest unsupported? The generator names the first construct it
    # refused; the histogram says which feature would move the most tests.
    reasons = {}
    for name in buckets["unsupported"]:
        what = details[name][2]
        what = what.split("unsupported: ", 1)[1] if "unsupported: " in what else what
        reasons[what] = reasons.get(what, 0) + 1
    if reasons:
        print("\n  unsupported, by first refused construct:")
        for what, n in sorted(reasons.items(), key=lambda kv: -kv[1])[:12]:
            print(f"  {n:5}  {what}")

    print()
    for k in ("pass", "fail", "error", "unsupported", "notyetpassing"):
        print(f"  {k:<13} {len(buckets[k]):4}")
    print(f"SCOREBOARD: {len(passing)} / {total} pass "
          f"({len(buckets['fail'])} fail, {len(buckets['error'])} error, "
          f"{len(buckets['unsupported'])} unsupported, {len(buckets['notyetpassing'])} notyetpassing)")

    ok = True
    if regressions:
        ok = False
        print(f"REGRESSION: {len(regressions)} test(s) in {args.expected} no longer pass:")
        for n in regressions:
            b, want, got = details.get(n, ("missing", "", ""))
            print(f"  {n}: {b}, want {want!r} got {got!r}")
    if new:
        print(f"NEW PASS: {len(new)} test(s) not yet in {args.expected}:")
        for n in new:
            print(f"  {n}")
        if args.update:
            kept = (expected - seen) | passing   # entries this run did not look at stay
            with open(args.expected, "w") as f:
                f.write("# Tests the 65816 VM passes. scripts/scoreboard.py --update maintains it;\n")
                f.write("# checks.scoreboard fails if any of these stops passing.\n")
                for n in sorted(kept):
                    f.write(n + "\n")
            print(f"  written to {args.expected}")
        else:
            print("  run with --update (just scoreboard-update) to claim them")
    if not passing:
        ok = False
        print("SCOREBOARD FAIL: nothing passes")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
