#!/usr/bin/env python3
"""The scoreboard: how much of the tvix evaluation corpus does the 65816 VM pass?

For every eval-okay-*.nix under each --corpus root:
  1. tools/nbc compiles it            -> rc 2 means UNSUPPORTED (outside the slice)
  2. a2min --run-nbc runs it on the VM -> prints `result: <value>` or `error: <E_*>`
  3. the printed value is compared with the .exp file

Two more kinds, both ours (the corpus has no .exp for its eval-fail tests): an
eval-fail-*.nix is a program Nix rejects too, and a refuse-*.nix is VALID Nix
the VM declines with E_UNSUP rather than answer wrongly (docs/DERIVATION.md
"Refused"). Their .exp is the error line the VM must print, e.g.
`error: E_UNSUP`; a value, or a different error, does not pass.

Buckets: pass, fail (ran, wrong value), error (VM aborted), gsos (refused at
run time by a builtin that needs GS/OS -- files, the environment, the network;
DECISIONS.md D26 -- which no slice of the generator can move), unsupported,
notyetpassing (tvix itself does not pass these; scored separately).

Where tvix's .exp disagrees with C++ Nix 2.34 -- the host, whose answers are
the truth here (D7) -- the host's answer is in --host-exp (tests/corpus/
host-exp/<corpus>/<test>.exp) and is scored instead; the vendored corpus is
never edited. Each one is listed in the output, and one that agrees with
tvix's .exp after all is an error, so the directory cannot rot.

--expected FILE is the ratchet: every test listed must still pass or the run
fails; newly passing tests are reported and, with --update, written back. This is
what makes the headline number only able to go up.

Compiled fixtures are left in --build next to a copy of their source so that
scripts/fixture-purity.sh can check each one against what it was compiled from.
"""
import argparse
import concurrent.futures
import os
import shutil
import subprocess
import sys
import time


def run(cmd, timeout=120, env=None):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return -1, "", "timeout"


def gsos_env(nix):
    """GS/OS for a test (docs/GSOS.md): its own directory mounted at /pwd/lang,
    the fixture convention's, and TEST_VAR=foo as a shell variable, as Nix's
    tests/functional/lang.sh exports it for eval-okay-getenv. A refuse-gsos-*
    fixture runs with no GS/OS at all: it tests D26's refusal by name."""
    env = dict(os.environ)
    if os.path.basename(nix).startswith("refuse-gsos-"):
        env.pop("A2MIN_MOUNT", None)
        return env
    env["A2MIN_MOUNT"] = "/pwd/lang=" + os.path.dirname(os.path.abspath(nix))
    env["A2MIN_VAR_TEST_VAR"] = "foo"
    return env


def find_tests(root):
    out = []
    for dirpath, _, files in os.walk(root):
        for f in sorted(files):
            if f.startswith(("eval-okay-", "eval-fail-", "refuse-")) and f.endswith(".nix"):
                path = os.path.join(dirpath, f)
                exp = path[:-4] + ".exp"
                if not os.path.exists(exp):
                    continue
                rel = os.path.relpath(path, root)[:-4]
                nyp = "notyetpassing" in rel.split(os.sep)
                out.append((rel, path, exp, nyp))
    return sorted(out)


def vm_result(stdout):
    """The harness prints exactly one of these lines. Split at newlines only:
    splitlines() also breaks at a form feed, which a printed string may hold
    raw, as Nix prints it (fromJSON's "\f")."""
    for line in stdout.split("\n"):
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
    ap.add_argument("--host-exp", default="tests/corpus/host-exp",
                    help="the host's answers where tvix's .exp disagrees (D7)")
    ap.add_argument("--verbose", "-v", action="store_true")
    ap.add_argument("--gc-stress", action="store_true",
                    help="collect the nursery at every bytecode boundary (slow; a collector test)")
    ap.add_argument("--jobs", "-j", type=int,
                    default=int(os.environ.get("NIX_BUILD_CORES") or 0) or os.cpu_count() or 1,
                    help="tests run side by side (default: NIX_BUILD_CORES, else every core)")
    args = ap.parse_args()
    run_mode = "--run-nbc-gc-stress" if args.gc_stress else "--run-nbc"

    os.makedirs(args.build, exist_ok=True)
    expected = set()
    if os.path.exists(args.expected):
        with open(args.expected) as f:
            expected = {l.strip() for l in f if l.strip() and not l.startswith("#")}

    buckets = {k: [] for k in ("pass", "fail", "error", "gsos", "unsupported", "notyetpassing")}
    details = {}
    seconds = {}

    def one(test):
        name, nix, exp, nyp = test
        t0 = time.monotonic()
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
            rc, out, err = run([args.a2min, run_mode, args.vm, base + ".nbc"], timeout=600, env=gsos_env(nix))
            kind, got = vm_result(out)
            if kind == "error":
                got = "error: " + got
                if got == want:
                    bucket = "pass"
                elif got.startswith("error: E_GSOS"):
                    bucket = "gsos"
                else:
                    bucket = "error"
            elif got == want:
                bucket = "pass"
            else:
                bucket = "fail"
        if nyp and bucket != "pass":
            bucket = "notyetpassing"
        return name, bucket, want, got, time.monotonic() - t0

    tests = []
    overridden = []
    for root in args.corpus:
        if not os.path.isdir(root):
            continue
        tag = os.path.basename(root.rstrip("/"))
        for rel, nix, exp, nyp in find_tests(root):
            host = os.path.join(args.host_exp, tag, rel + ".exp")
            if os.path.exists(host):
                with open(exp) as a, open(host) as b:
                    if a.read().strip() == b.read().strip():
                        print(f"HOST-EXP STALE: {host} agrees with {exp}; delete it")
                        return 1
                overridden.append(f"{tag}/{rel}")
                exp = host
            tests.append((f"{tag}/{rel}", nix, exp, nyp))
    for n in overridden:
        print(f"  scored against the host's answer, where tvix's .exp disagrees: {n}")

    # Every test is its own a2min process (~6 MB) with its own files, so they
    # run side by side; map() keeps the report in corpus order regardless.
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for name, bucket, want, got, secs in pool.map(one, tests):
            buckets[bucket].append(name)
            details[name] = (bucket, want, got)
            seconds[name] = secs
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

    # The GS/OS refusals, by builtin: what L7 will move.
    gsos = {}
    for name in buckets["gsos"]:
        what = details[name][2].split("E_GSOS", 1)[1].strip()
        gsos[what] = gsos.get(what, 0) + 1
    if gsos:
        print("\n  need GS/OS, by builtin:")
        for what, n in sorted(gsos.items(), key=lambda kv: -kv[1]):
            print(f"  {n:5}  {what}")

    # Always name the errors: a count alone says nothing about what to fix.
    if buckets["error"]:
        print("\n  errors:")
        for n in buckets["error"]:
            print(f"    {n}: {details[n][2][:100]}")
    # And where the wall clock went (D19: count before guessing).
    if seconds:
        print(f"\n  slowest (of {sum(seconds.values()):.0f} s total, {args.jobs} jobs):")
        for n, s in sorted(seconds.items(), key=lambda kv: -kv[1])[:8]:
            print(f"  {s:7.1f} s  {n}  [{details[n][0]}]")

    print()
    for k in ("pass", "fail", "error", "gsos", "unsupported", "notyetpassing"):
        print(f"  {k:<13} {len(buckets[k]):4}")
    print(f"SCOREBOARD: {len(passing)} / {total} pass "
          f"({len(buckets['fail'])} fail, {len(buckets['error'])} error, "
          f"{len(buckets['gsos'])} need GS/OS, "
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
