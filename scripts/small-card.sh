#!/usr/bin/env bash
# The fixtures that fill a card's half, on a card small enough that they do
# (docs/HEAP.md "Long lists", step 0).
#
# a2min's card has 8 MB halves, as MAME's and rev A's do, and a plain run of
# eval-okay-mature-collection never fills one: its mature space is then never
# collected, and the fixture tests nothing it was written for. So it runs here
# on 512 KB halves, the size it was written against, where its rounds promote
# several halves' worth, and must both answer as the host does and collect
# mature space at least MIN_MATURE times (3, as measured at 512 KB).
#
# Usage: small-card.sh <a2min> <nbc> <cvm.bin>
set -uo pipefail

A2MIN="$1"; NBC="$2"; VM="$3"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
MIN_MATURE=3
T=eval-okay-mature-collection

"$NBC" "tests/fixtures/$T.nix" -o "$WORK/$T.nbc" || { echo "cannot compile $T" >&2; exit 1; }
want=$(tr -d '\n' < "tests/fixtures/$T.exp")
out=$(A2MIN_CARD_HALF=0x80000 A2MIN_PRINT=1 "$A2MIN" --run-nbc "$VM" "$WORK/$T.nbc" 2>&1)
got=$(printf '%s\n' "$out" | sed -n 's/^result: //p; /^error: /p' | head -1)
mature=$(printf '%s\n' "$out" | sed -n 's/^  phases: .*, \([0-9]*\) of mature space.*/\1/p')
echo "  $T: ${got:0:80}; ${mature:-no} collections of mature space"
[ "$got" = "$want" ] || { echo "SMALL CARD FAILED: $T answered '${got:0:80}', not '$want'" >&2; exit 1; }
[ -n "$mature" ] && [ "$mature" -ge "$MIN_MATURE" ] || {
    echo "SMALL CARD FAILED: $T collected mature space ${mature:-no} times, under $MIN_MATURE" >&2; exit 1; }
echo "SMALL CARD OK: $T fills a 512 KB half and collects it $mature times"
