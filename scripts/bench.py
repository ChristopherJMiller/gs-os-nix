#!/usr/bin/env python3
"""The interpreter's speed, held to a ratchet (DECISIONS.md D29, VM.md "Speed").

Each tests/bench/<name>.nix is compiled by nbc and run once on the VM under
a2min, with the card, as programs normally run. Its answer must be the host's
(<name>.exp, from nix-instantiate: D7), and its CPU cycles may not exceed what
tests/bench/cycles.txt records. a2min is cycle-exact and deterministic, so a
change of one cycle either way is real and is reported: slower fails; faster
passes and asks for --update, which records the new numbers -- as
expected-pass.txt holds the scoreboard and bank0.txt holds bank 0.

Usage: bench.py --nbc NBC --a2min A2MIN --vm CVM.BIN [--dir tests/bench]
                [--record tests/bench/cycles.txt] [--update]
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile


def run(args, name, nbc_dir):
    nix = os.path.join(args.dir, name + ".nix")
    nbc = os.path.join(nbc_dir, name + ".nbc")
    subprocess.run([args.nbc, nix, "-o", nbc], check=True, capture_output=True)
    p = subprocess.run([args.a2min, "--run-nbc", args.vm, nbc], capture_output=True, text=True, timeout=900)
    out = p.stdout + p.stderr
    m = re.search(r"(\d+) instructions, (\d+) CPU cycles, (\d+) slot accesses", out)
    got = next((l[len("result: "):] if l.startswith("result: ") else l
                for l in out.split("\n") if l.startswith("result: ") or l.startswith("error: ")), "")
    if not m:
        return None, got
    return (int(m.group(2)), int(m.group(1)), int(m.group(3))), got


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nbc", required=True)
    ap.add_argument("--a2min", required=True)
    ap.add_argument("--vm", required=True)
    ap.add_argument("--dir", default="tests/bench")
    ap.add_argument("--record", default=None)
    ap.add_argument("--update", action="store_true")
    args = ap.parse_args()
    record = args.record or os.path.join(args.dir, "cycles.txt")

    recorded = {}
    if os.path.exists(record):
        for line in open(record):
            if line.strip() and not line.startswith("#"):
                name, cycles = line.split()[:2]
                recorded[name] = int(cycles)

    names = sorted(f[:-4] for f in os.listdir(args.dir) if f.endswith(".nix"))
    results, wrong, slower, faster = {}, [], [], []
    with tempfile.TemporaryDirectory() as tmp:
        print(f"{'benchmark':<16} {'cycles':>14} {'recorded':>14} {'change':>9} {'instructions':>13} {'bus':>8}")
        for name in names:
            stats, got = run(args, name, tmp)
            want = open(os.path.join(args.dir, name + ".exp")).read().strip()
            if stats is None or got != want:
                wrong.append(name)
                print(f"{name:<16} wrong: got {got[:60]!r}")
                continue
            cycles, instructions, bus = stats
            results[name] = cycles
            old = recorded.get(name)
            change = "new" if old is None else f"{100.0 * (cycles - old) / old:+.2f}%"
            shown = "-" if old is None else f"{old:,}"
            print(f"{name:<16} {cycles:>14,} {shown:>14} {change:>9} {instructions:>13,} {bus:>8,}")
            if old is not None and cycles > old:
                slower.append(name)
            elif old is None or cycles < old:
                faster.append(name)
    total = sum(results.values())
    print(f"{'total':<16} {total:>14,}")

    if wrong:
        print(f"BENCH FAILED: {len(wrong)} benchmark(s) did not give the host's answer: {' '.join(wrong)}")
        return 1
    if args.update:
        with open(record, "w") as f:
            f.write("# CPU cycles per benchmark (scripts/bench.py; rewritten by --update).\n")
            f.write("# a2min is cycle-exact: slower fails the check, faster asks for --update.\n")
            for name in names:
                f.write(f"{name} {results[name]}\n")
        print(f"recorded {len(results)} benchmarks, {total:,} cycles in all, to {record}")
        return 0
    if slower:
        print(f"BENCH FAILED: slower than recorded: {' '.join(slower)} (account for it, then --update)")
        return 1
    missing = sorted(set(recorded) - set(results))
    if missing:
        print(f"BENCH FAILED: recorded but gone: {' '.join(missing)} (--update)")
        return 1
    if faster:
        print(f"BENCH OK, and faster or new: {' '.join(faster)} -- record it with --update")
    else:
        print("BENCH OK: every benchmark as recorded")
    return 0


if __name__ == "__main__":
    sys.exit(main())
