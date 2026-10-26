#!/usr/bin/env bash
# checks.float-vectors: target/vm/c/float.c, built by ORCA/C and run on a2min,
# against the host's own answers (docs/FLOAT.md). scripts/float-vectors.c
# writes, per seed, 1,000 arithmetic vectors -- add, sub, mul, div, ==, <,
# int -> float, floor, ceil, each answered by the host's IEEE hardware -- and
# 120 %f vectors answered by its glibc printf, the libc Nix prints with;
# target/tests/float_test.c recomputes every one with float.c and counts the
# wrong ones. Then the same with a rounding bug planted in a copy of float.c
# (ties away from zero, not to even), which must be caught (D14).
#
# Usage: float-vectors.sh <a2min> [seeds...]   (repo root; cc and orcabuild on PATH)
set -uo pipefail

A2MIN=$1
shift
SEEDS=${*:-1 2 3}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cc -O2 -o "$WORK/fv" scripts/float-vectors.c -lm || { echo "the generator does not build" >&2; exit 1; }

# run <float.c> <seed>: prints a2min's result line.
run() {
    "$WORK/fv" "$2" > "$WORK/float_vectors.c"
    orcabuild -o "$WORK/ft.bin" -O -I target/vm/c target/tests/float_test.c \
        "$WORK/float_vectors.c" "$1" > "$WORK/build.log" 2>&1 || { echo "DID NOT BUILD"; return; }
    "$A2MIN" --run-bin "$WORK/ft.bin" 2>&1 | grep -a '^result:\|^a2min:' | head -1
}

echo "=== float.c against the host's IEEE-754 and printf (docs/FLOAT.md) ==="
fail=0
for s in $SEEDS; do
    r=$(run target/vm/c/float.c "$s")
    case "$r" in
        "result: 00 00 FF FF"*) echo "  seed $s: 1,120 vectors, all right" ;;
        *) echo "  seed $s: WRONG -- $r (wrong count, then the first: \$8000+i a format)"; fail=1 ;;
    esac
done

# D14: a planted rounding bug must be caught.
cp target/vm/c/float.c "$WORK/planted.c"
sed -i 's/^    if (round_bits == 0x200) sig &= ~(uint64_t)1;  \/\* a tie: to even \*\/$//' "$WORK/planted.c"
if cmp -s target/vm/c/float.c "$WORK/planted.c"; then
    echo "  plant: PATTERN MISS -- update this script"; fail=1
else
    r=$(run "$WORK/planted.c" 1)
    case "$r" in
        "result: 00 00 FF FF"*) echo "  plant (ties away from even): ACCEPTED -- the vectors are decorative"; fail=1 ;;
        "result: "*) echo "  plant (ties away from even): caught -- $r" ;;
        *) echo "  plant (ties away from even): FAILED FOR ANOTHER REASON -- $r"; fail=1 ;;
    esac
fi
echo
[ "$fail" -eq 0 ] || exit 1
echo "FLOAT VECTORS OK: float.c is the host's arithmetic and printf, bit for bit"
