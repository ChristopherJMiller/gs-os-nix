#!/usr/bin/env bash
# The card's geometry where it matters (docs/HEAP.md "Long lists", step 0).
#
# The fixtures that fill a card's half, on a card small enough that they do.
# a2min's card has 8 MB halves, as MAME's and rev A's do, and a plain run of
# eval-okay-mature-collection never fills one: its mature space is then never
# collected, and the fixture tests nothing it was written for. So it runs here
# on 512 KB halves, the size it was written against, where its rounds promote
# several halves' worth, and must both answer as the host does and collect
# mature space at least MIN_MATURE times (3, as measured at 512 KB).
#
# On 8 MB halves, eval-okay-card-heap-big (~1.2 MB live) must answer without
# collecting mature space at all: a VM that took its half for smaller than the
# card's would collect early, and answer right, so only the count shows it.
# And under mature stress alone -- a collection after every promotion, not a
# nursery collection at every boundary -- it moves over 512 KB of live data
# on an 8 MB card at least MIN_MOVES times (20 measured), both flips, and
# must still answer right.
#
# A card that cannot report its half -- one built before GEOMETRY, which
# answers it E_BADVERB (a2min A2MIN_CARD_REFUSE) -- ends the run with
# E_NOCARD. The VM asks at card_find, which once ran before the run's error
# exit was set up, and longjmp'd through an unset buffer.
#
# And a2min refuses an A2MIN_CARD_HALF it cannot build a card of, rather than
# reading a prefix of it or rounding it.
#
# Usage: small-card.sh <a2min> <nbc> <cvm.bin>
set -uo pipefail

A2MIN="$1"; NBC="$2"; VM="$3"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
MIN_MATURE=3
MIN_MOVES=10
T=eval-okay-mature-collection
B=eval-okay-card-heap-big
fail() { echo "SMALL CARD FAILED: $*" >&2; exit 1; }

for t in $T $B; do
    "$NBC" "tests/fixtures/$t.nix" -o "$WORK/$t.nbc" || { echo "cannot compile $t" >&2; exit 1; }
done
# run <fixture> [VAR=value ...]: a2min's answer line, then its mature collections
run() {
    local t=$1 out; shift
    out=$(env "$@" A2MIN_PRINT=1 "$A2MIN" --run-nbc "$VM" "$WORK/$t.nbc" 2>&1)
    printf '%s\n' "$(printf '%s\n' "$out" | sed -n 's/^result: //p; /^error: /p' | head -1)"
    printf '%s\n' "$(printf '%s\n' "$out" | sed -n 's/^  phases: .*, \([0-9]*\) of mature space.*/\1/p' | head -1)"
    printf '%s\n' "$(printf '%s\n' "$out" | tail -1)"
}

{ read -r got; read -r mature; read -r last; } < <(run $T A2MIN_CARD_HALF=0x80000)
echo "  $T on 512 KB halves: ${got:0:80}; ${mature:-no} collections of mature space"
[ "$got" = "$(tr -d '\n' < tests/fixtures/$T.exp)" ] || fail "$T answered '${got:-$last}'"
[ -n "$mature" ] && [ "$mature" -ge "$MIN_MATURE" ] || fail "$T collected mature space ${mature:-no} times, under $MIN_MATURE"

{ read -r got; read -r mature; read -r last; } < <(run $B)
echo "  $B on 8 MB halves: ${got:0:80}; ${mature:-no} collections of mature space"
[ "$got" = "$(tr -d '\n' < tests/fixtures/$B.exp)" ] || fail "$B answered '${got:-$last}'"
[ "$mature" = 0 ] || fail "$B collected mature space ${mature:-no} times on 8 MB halves, where it fits: the VM takes its half for smaller than the card's"

{ read -r got; read -r mature; read -r last; } < <(run $B A2MIN_MATURE_STRESS=1)
echo "  $B on 8 MB halves, mature stress: ${got:0:80}; ${mature:-no} collections of mature space"
[ "$got" = "$(tr -d '\n' < tests/fixtures/$B.exp)" ] || fail "$B under mature stress answered '${got:-$last}'"
[ -n "$mature" ] && [ "$mature" -ge "$MIN_MOVES" ] || fail "$B under mature stress collected ${mature:-no} times, under $MIN_MOVES"

{ read -r got; read -r mature; read -r last; } < <(run $T A2MIN_CARD_REFUSE=0x26)
echo "  $T on a card without GEOMETRY: ${got:-$last}"
[ "$got" = "error: E_NOCARD" ] || fail "a card without GEOMETRY gave '${got:-$last}', not E_NOCARD"

for h in 0x80000junk 524288.9 0x90000 0x40000 0x1000000 -0x80000; do
    out=$(A2MIN_CARD_HALF="$h" "$A2MIN" --run-nbc "$VM" "$WORK/$T.nbc" 2>&1); rc=$?
    [ $rc -eq 2 ] && printf '%s\n' "$out" | grep -q "^A2MIN_CARD_HALF must be" \
        || fail "A2MIN_CARD_HALF=$h was not refused (exit $rc: $(printf '%s\n' "$out" | tail -1))"
done
echo "  A2MIN_CARD_HALF refused: 0x80000junk 524288.9 0x90000 0x40000 0x1000000 -0x80000"

echo "SMALL CARD OK: $T fills a 512 KB half and collects it $MIN_MATURE+ times; $B fits an 8 MB one and moves over 512 KB live; a card without GEOMETRY is E_NOCARD; a bad A2MIN_CARD_HALF is refused"
