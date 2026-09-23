#!/usr/bin/env python3
"""Verb economics: does each card verb pay for its bus crossing on real programs?

The project's hard gate (docs/DECISIONS.md, docs/CARD-ISA.md) is that a verb must
be justified by a measured cycle-count comparison against its 65816 equivalent.
The micro-benchmarks did that one verb at a time on synthetic operands. This does
it on the corpus: the same programs, run once with every verb on the card and
again with one verb forced onto the CPU, and the difference is what that verb is
worth.

The switches are the VM's, not the harness's: A2MIN_VERB_MASK sets the byte at
$0331 (vm.h VERB_NO_*), which makes the VM take its own CPU path for that verb
while everything else stays on the card; A2MIN_NO_CARD hides the card entirely.
So each row runs the same bytecode over the same heap -- only the implementation
of one operation moves.

Two things to read carefully in the output:

  - It runs with a PROMOTION AT EVERY BOUNDARY by default (--gc-stress, the
    default here). Without it the 64 KB nursery never fills on a fixture,
    nothing is ever promoted, and LOOKUP is never called at all -- the masked
    and unmasked runs would be identical and the measurement would say nothing.
  - The `no card at all` row is NOT a verdict on the card. It is cheaper in
    cycles because it never crosses the bus, but it keeps the whole heap in
    64 KB of RAM, which is the one thing a IIgs cannot do for a real Nix
    evaluation. That row prices D6's bus traffic; it does not price D6.
"""
import argparse
import os
import re
import subprocess
import sys

# name -> (environment overrides, what the row means)
CONFIGS = [
    ("card, all verbs", {}, "the baseline: LOOKUP and ALU64 on the card"),
    ("LOOKUP on the CPU", {"A2MIN_VERB_MASK": "1"}, "find_attr scans the copy instead"),
    ("ALU64 on the CPU", {"A2MIN_VERB_MASK": "2"}, "ORCA/C's own 64-bit mul/div"),
    ("both on the CPU", {"A2MIN_VERB_MASK": "3"}, "the card holds memory, nothing else"),
    ("no card at all", {"A2MIN_NO_CARD": "1"}, "no promotion either: heap stays in RAM"),
]

# Which mask bit each verb's row uses, for the per-verb verdict.
VERBS = [("LOOKUP", "LOOKUP on the CPU"), ("ALU64", "ALU64 on the CPU")]

STATS = re.compile(r"(\d+) instructions, (\d+) CPU cycles, (\d+) slot accesses")


def run_one(a2min, vm, nbc, gc_stress, env_extra, budget):
    env = dict(os.environ)
    env["A2MIN_BUDGET"] = str(budget)
    env.update(env_extra)
    mode = "--run-nbc-gc-stress" if gc_stress else "--run-nbc"
    try:
        p = subprocess.run([a2min, mode, vm, nbc], capture_output=True, text=True,
                           timeout=600, env=env)
    except subprocess.TimeoutExpired:
        return None, "timeout"
    m = STATS.search(p.stdout)
    answer = None
    for line in p.stdout.splitlines():
        if line.startswith("result: ") or line.startswith("error: "):
            answer = line.strip()
    if not m or answer is None:
        return None, "no stats line"
    return (int(m.group(1)), int(m.group(2)), int(m.group(3))), answer


def crossover(args, sizes, stress):
    """Where does the CAM start paying?

    The verb was justified on 32-attribute batches (docs/HEAP.md); the corpus
    has sets of two to five. So the useful number is not a verdict on one
    workload, it is the set size at which LOOKUP overtakes a CPU hash scan.
    Each program selects the LAST attribute of an n-attribute set -- the worst
    case for a scan and the best for the CAM -- so this is the verb's most
    favourable reading.
    """
    print(f"\n  LOOKUP crossover: selecting the last of n attributes, "
          f"{'mature' if stress else 'young'} set\n")
    # The totals are dominated by promotion (under stress the whole set crosses
    # the bus at every boundary), so the columns that mean anything are the
    # DIFFERENCES: what the verb saves in cycles and what it costs in accesses.
    print(f"  {'n':>6} {'CAM cycles':>12} {'scan cycles':>12} "
          f"{'verb saves':>11} {'verb bus':>9} {'verdict':>11}")
    ok = True
    # Its own directory: these are generated scratch, not fixtures, and must not
    # end up in a purity scan of the compiled corpus.
    gen = os.path.join(args.nbc_dir, "crossover")
    os.makedirs(gen, exist_ok=True)
    for n in sizes:
        names = [f"a{i:04d}" for i in range(n)]
        body = " ".join(f"{nm} = {i};" for i, nm in enumerate(names))
        src = os.path.join(gen, f"lookup{n}.nix")
        out = os.path.join(gen, f"lookup{n}.nbc")
        with open(src, "w") as f:
            f.write(f"let s = {{ {body} }}; in s.{names[-1]}\n")
        rc, _, err = run_nbc_tool(args.nbc, src, out)
        if rc != 0:
            print(f"  {n:>6}  generator rc={rc}: {err.strip()}")
            ok = False
            continue
        cam, a1 = run_one(args.a2min, args.vm, out, stress, {}, args.budget)
        scan, a2 = run_one(args.a2min, args.vm, out, stress,
                           {"A2MIN_VERB_MASK": "1"}, args.budget)
        if cam is None or scan is None:
            print(f"  {n:>6}  did not run")
            ok = False
            continue
        want = f"result: {n - 1}"
        if a1 != want or a2 != want:
            print(f"  {n:>6}  WRONG: CAM {a1!r}, scan {a2!r}, want {want!r}")
            ok = False
            continue
        saves = scan[1] - cam[1]
        verdict = "CAM wins" if saves > 0 else "scan wins"
        print(f"  {n:>6} {cam[1]:>12,} {scan[1]:>12,} {saves:>+11,} "
              f"{cam[2] - scan[2]:>+9,} {verdict:>11}")
    return ok


def run_nbc_tool(nbc, src, out):
    try:
        p = subprocess.run([nbc, src, "-o", out], capture_output=True, text=True,
                           timeout=120)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return -1, "", "timeout"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a2min", required=True)
    ap.add_argument("--vm", required=True, help="the flattened VM (target/build/cvm.bin)")
    ap.add_argument("--nbc-dir", required=True, help="compiled .nbc files to use as the workload")
    ap.add_argument("--budget", type=int, default=400_000_000)
    ap.add_argument("--no-gc-stress", action="store_true",
                    help="run without promotion at every boundary -- see the docstring")
    ap.add_argument("--per-test", action="store_true", help="also print every test")
    ap.add_argument("--only", default="",
                    help="comma-separated substrings; keep only matching programs. The "
                         "check uses this to pick the ones that actually exercise the "
                         "verbs, rather than paying for 40 stress runs five times over")
    ap.add_argument("--crossover", default="",
                    help="comma-separated attribute-set sizes: also measure where LOOKUP "
                         "overtakes a CPU hash scan. Needs --nbc")
    ap.add_argument("--nbc", default="", help="the fixture generator, for --crossover")
    args = ap.parse_args()
    if args.crossover and not args.nbc:
        print("--crossover needs --nbc", file=sys.stderr)
        return 1

    stress = not args.no_gc_stress
    tests = sorted(f for f in os.listdir(args.nbc_dir) if f.endswith(".nbc"))
    if args.only:
        want = [s.strip() for s in args.only.split(",") if s.strip()]
        tests = [t for t in tests if any(w in t for w in want)]
        missing = [w for w in want if not any(w in t for t in tests)]
        if missing:
            print(f"--only matched nothing: {', '.join(missing)}", file=sys.stderr)
            return 1
    if not tests:
        print(f"no .nbc files in {args.nbc_dir}", file=sys.stderr)
        return 1

    # totals[config] = [instructions, cycles, slot accesses]; answers[(config, test)]
    totals = {name: [0, 0, 0] for name, _, _ in CONFIGS}
    answers = {}
    disagreements = []
    ok = True

    for t in tests:
        path = os.path.join(args.nbc_dir, t)
        for name, env_extra, _ in CONFIGS:
            stats, answer = run_one(args.a2min, args.vm, path, stress, env_extra, args.budget)
            if stats is None:
                print(f"  {name:<18} {t}: {answer}", file=sys.stderr)
                ok = False
                continue
            for i in range(3):
                totals[name][i] += stats[i]
            answers[(name, t)] = answer
            if args.per_test:
                print(f"  {name:<18} {t:<52} {stats[1]:>12,} cycles {stats[2]:>8,} bus")
        # Moving one operation to the CPU must not change the answer. If it does,
        # one of the two implementations is wrong, and that matters far more than
        # the cycle counts.
        base = answers.get((CONFIGS[0][0], t))
        for name, _, _ in CONFIGS[1:]:
            got = answers.get((name, t))
            if got is not None and base is not None and got != base:
                disagreements.append((t, name, base, got))
                ok = False

    print(f"\nverb economics: {len(tests)} programs, "
          f"{'a promotion at every bytecode boundary' if stress else 'normal allocation'}\n")
    print(f"  {'configuration':<18} {'instructions':>14} {'cycles':>14} "
          f"{'vs baseline':>12} {'bus':>9}")
    bl = totals[CONFIGS[0][0]]
    for name, _, why in CONFIGS:
        v = totals[name]
        delta = "--" if name == CONFIGS[0][0] else f"{(v[1] - bl[1]) / bl[1] * 100:+.1f}%"
        print(f"  {name:<18} {v[0]:>14,} {v[1]:>14,} {delta:>12} {v[2]:>9,}")
        print(f"  {'':<18} {why}")

    print("\n  what each verb is worth on this workload:")
    for verb, row in VERBS:
        v = totals[row]
        dc = v[1] - bl[1]
        db = bl[2] - v[2]
        if dc == 0 and db == 0:
            verdict = "never reached on this workload -- nothing to price"
        elif dc <= 0:
            verdict = (f"COSTS {-dc:,} cycles and {db:,} bus accesses -- it does not pay "
                       f"on this workload")
        else:
            verdict = f"saves {dc:,} cycles ({dc / bl[1] * 100:.1f}%) for {db:,} bus accesses"
        print(f"    {verb:<8} {verdict}")

    nc = totals["no card at all"]
    dc = nc[1] - bl[1]
    print(f"\n  D6, for reference: against keeping the whole heap in RAM, the card costs "
          f"{bl[2]:,}\n  bus accesses and {abs(dc):,} cycles "
          f"{'LESS' if dc > 0 else 'more'} "
          f"({abs(dc) / bl[1] * 100:.0f}%).")
    if dc > 0 and stress:
        print("  That direction is a GC-stress artifact, not a claim: with a collection at\n"
              "  every boundary and no card to promote to, the copy-back path re-copies the\n"
              "  whole live graph every instruction, where promotion empties the nursery and\n"
              "  leaves the next collection almost nothing to do. The card buys a heap larger\n"
              "  than the machine (docs/HEAP.md); do not read this row as buying speed.")

    if args.crossover:
        sizes = [int(s) for s in args.crossover.split(",") if s.strip()]
        if not crossover(args, sizes, stress):
            ok = False

    if disagreements:
        print("\nDISAGREEMENT: moving an operation to the CPU changed the answer.")
        for t, name, base, got in disagreements:
            print(f"  {t}: baseline {base!r}, {name} {got!r}")
    print("\nVERB ECONOMICS OK" if ok else "\nVERB ECONOMICS FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
