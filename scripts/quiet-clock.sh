#!/usr/bin/env bash
# a2min clocks the card only while clocking it can change something (the
# "quiet clock", sim/a2min.cpp advance()). That is exact by construction, and
# this proves it: every program below runs twice, with the quiet clock and with
# A2MIN_EXACT_CLOCK=1 (every card cycle simulated), and the two outputs must be
# byte-identical -- instructions, CPU cycles, slot accesses, phase counters,
# results. Cycle counts are the project's instrument (D5, D19), so "the same
# answer, slightly different timing" would be a failure too.
#
# Then it proves it can fail (D14): an a2min whose card_write does not end the
# quiet period is built from a COPY of the sources, and the same comparison
# must catch it.
#
# Usage: quiet-clock.sh <a2min> <cvm.bin> <nbc-dir> <m2-bin> <vectors>
set -uo pipefail

A2MIN="$1"; VM="$2"; NBCDIR="$3"; M2BIN="$4"; VECTORS="$5"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# compare <a2min> [first]: 0 if every run is identical both ways, 1 otherwise.
# With "first", stop at the first difference (the self-test needs only one).
compare() {
    local bin="$1" first="${2:-}" f mask n=0 bad=0
    for f in "$NBCDIR"/*.nbc; do
        # Mask 0 only: masks 1 and 2 move work OFF the card, so mask 0 is the
        # card-heaviest case, and the exact half of every run costs the old
        # full-clock time (all three masks made this check 16 minutes).
        for mask in 0; do
            A2MIN_EXACT_CLOCK=0 A2MIN_VERB_MASK=$mask A2MIN_BUDGET=200000000 "$bin" --run-nbc "$VM" "$f" > "$WORK/fast" 2>&1
            A2MIN_EXACT_CLOCK=1 A2MIN_VERB_MASK=$mask A2MIN_BUDGET=200000000 "$bin" --run-nbc "$VM" "$f" > "$WORK/exact" 2>&1
            n=$((n + 1))
            cmp -s "$WORK/fast" "$WORK/exact" || { bad=$((bad + 1)); echo "  differs: $(basename "$f") mask=$mask"; }
            [ -n "$first" ] && [ "$bad" -gt 0 ] && { echo "  $n runs compared, stopped at the first difference"; return 1; }
        done
    done
    A2MIN_EXACT_CLOCK=0 "$bin" "$M2BIN" "$VECTORS" 4 > "$WORK/fast" 2>&1
    A2MIN_EXACT_CLOCK=1 "$bin" "$M2BIN" "$VECTORS" 4 > "$WORK/exact" 2>&1
    n=$((n + 1))
    cmp -s "$WORK/fast" "$WORK/exact" || { bad=$((bad + 1)); echo "  differs: M2 store path"; }
    echo "  $n runs compared, $bad differ"
    [ "$bad" -eq 0 ]
}

echo "=== quiet clock: fast vs exact ==="
compare "$A2MIN" || { echo "QUIET CLOCK FAILED: skipping idle card cycles changed an output" >&2; exit 1; }

echo "=== self-test: a2min whose card_write leaves the card quiet ==="
mkdir -p "$WORK/src"
cp -r rtl sim "$WORK/src/"
sed -i '/^void card_write/,/^}/ s|    card_quiet = false;                   // an input changed||' "$WORK/src/sim/a2min.cpp"
if cmp -s sim/a2min.cpp "$WORK/src/sim/a2min.cpp"; then
    echo "  PATTERN MISS -- update this script" >&2; exit 1
fi
( cd "$WORK/src" && verilator --cc --exe --build -j "${NIX_BUILD_CORES:-4}" -Irtl --top-module a2nix_card \
      -Wall -Wno-DECLFILENAME --Mdir build -o a2min rtl/a2nix_card.v sim/a2min.cpp > build.log 2>&1 ) \
    || { echo "  the broken a2min did not build" >&2; tail "$WORK/src/build.log" >&2; exit 1; }
if compare "$WORK/src/build/a2min" first > "$WORK/selftest.log"; then
    echo "  SURVIVED -- the comparison cannot tell a broken quiet clock" >&2; exit 1
fi
tail -1 "$WORK/selftest.log"
echo "QUIET CLOCK OK: identical to the exact clock, and a broken one is caught"
