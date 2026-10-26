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
# NLOHMANN_INCLUDE: nlohmann/json's include directory (the devShell and the
# flake check set it), for toJSON's answers.
c++ -O2 -std=c++17 ${NLOHMANN_INCLUDE:+-I"$NLOHMANN_INCLUDE"} -o "$WORK/jf" scripts/json-floats.cpp \
    || { echo "json-floats does not build (NLOHMANN_INCLUDE?)" >&2; exit 1; }

# run <float.c> <seed>: a2min's result line -- of the arithmetic and %f
# vectors, and, if those are all right, of toJSON's (two binaries: their data
# does not fit one below $C000).
run() {
    local part r
    for part in arith json parse; do
        : > "$WORK/json.hex"
        "$WORK/fv" "$2" "$WORK/json.hex" "$part" > "$WORK/float_vectors.c"
        { echo "const char *const json_want[] = {"
          if [ -s "$WORK/json.hex" ]; then "$WORK/jf" < "$WORK/json.hex" | sed 's/.*/"&",/'; else echo '""'; fi
          echo "};"; } >> "$WORK/float_vectors.c"
        orcabuild -o "$WORK/ft.bin" -O -I target/vm/c target/tests/float_test.c \
            "$WORK/float_vectors.c" "$1" > "$WORK/build.log" 2>&1 || { echo "DID NOT BUILD ($part)"; return; }
        r=$("$A2MIN" --run-bin "$WORK/ft.bin" 2>&1 | grep -a '^result:\|^a2min:' | head -1)
        case "$r" in "result: 00 00 FF FF"*) ;; *) echo "$r ($part)"; return ;; esac
    done
    echo "result: 00 00 FF FF"
}

echo "=== float.c against the host's IEEE-754 and printf (docs/FLOAT.md) ==="
fail=0
for s in $SEEDS; do
    r=$(run target/vm/c/float.c "$s")
    case "$r" in
        "result: 00 00 FF FF"*) echo "  seed $s: 1,820 vectors, all right" ;;
        *) echo "  seed $s: WRONG -- $r (wrong count, then the first: \$8000+i a format, \$4000+i toJSON, \$2000+i strtod)"; fail=1 ;;
    esac
done

# D14: planted bugs must be caught. plant <name> <sed expression>
plant() {
    cp target/vm/c/float.c "$WORK/planted.c"
    sed -i "$2" "$WORK/planted.c"
    if cmp -s target/vm/c/float.c "$WORK/planted.c"; then
        echo "  plant ($1): PATTERN MISS -- update this script"; fail=1; return
    fi
    r=$(run "$WORK/planted.c" 1)
    case "$r" in
        "result: 00 00 FF FF"*) echo "  plant ($1): ACCEPTED -- the vectors are decorative"; fail=1 ;;
        "result: "*) echo "  plant ($1): caught -- $r" ;;
        *) echo "  plant ($1): FAILED FOR ANOTHER REASON -- $r"; fail=1 ;;
    esac
}
plant "ties away from even" 's/^    if (round_bits == 0x200) sig \&= ~(uint64_t)1;  \/\* a tie: to even \*\/$//'
plant "toJSON without Grisu2's rounding" 's/^        buf\[len - 1\]--;$/        ;/'
plant "strtod without its sticky bit" 's/^    return norm_round_pack(sign, 1084 - sh, q | (nd != 0));$/    return norm_round_pack(sign, 1084 - sh, q);/'
echo
[ "$fail" -eq 0 ] || exit 1
echo "FLOAT VECTORS OK: float.c is the host's arithmetic, printf, toJSON text and strtod, bit for bit"
