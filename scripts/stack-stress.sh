#!/usr/bin/env bash
# The C stack's checks are never too late (docs/VM.md "The C stack").
#
# The VM's stack runs down from STACK_TOP ($BFFF) to STACK_FLOOR ($7000),
# above the program; under the floor is bank 0's room to grow, then its
# globals (vm.h, D31). stack_check() and stack_need() refuse (E_NOMEM) once S
# is under STACK_MIN, 1 KB over the floor -- but only if every path between
# two checks fits that margin. When the stack was $0400-$1FFF, over the
# harness's flags and STATS, a path that did not wrote over the flags (turning
# GC stress and verb masks on mid-run) before any check saw it.
#
# So: each program below runs with its stack starting at every step from just
# over its own depth (a2min's "lowest S" with the whole stack) down to
# STACK_MIN (a2min A2MIN_STACK_TOP), which puts each of its operations at
# every depth near the floor, under a2min's tripwire (S under the floor, or a
# write to bank 0 that is nobody's, is a loud failure). Every run must give
# the program's answer or E_NOMEM, nothing else. The programs are the ones
# that go deepest between two checks, one per kind of deep path, as measured
# (VM.md), and the deepest nesting of each kind.
#
# Then it proves it can fail (D14): a VM built from a copy with the old margin
# (STACK_MIN 256 bytes over the floor) must be caught.
#
# Usage: stack-stress.sh <a2min> <nbc> <cvm.bin>   (orcabuild on PATH for the self-test)
set -uo pipefail

A2MIN="$1"; NBC="$2"; VM="$3"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
STEP=${STACK_STEP:-64}
hexdef() { sed -n "s/^#define $1 *0x\([0-9A-F]*\)U.*/\1/p" target/vm/c/vm.h; }
TOP=$(hexdef STACK_TOP); MIN=$(hexdef STACK_MIN)
[ -n "$TOP" ] && [ -n "$MIN" ] || { echo "cannot read STACK_TOP and STACK_MIN from vm.h" >&2; exit 1; }
TOP=$((16#$TOP)); MIN=$((16#$MIN))
# the most a stack_need() asks for over STACK_MIN: vm.h's FLOAT_STACK, TOML_STACK
NEED=$(sed -n 's/^#define [A-Z]*_STACK *\([0-9]*\)U.*/\1/p' target/vm/c/vm.h | sort -n | tail -1)
[ -n "$NEED" ] || { echo "cannot read the *_STACK sizes from vm.h" >&2; exit 1; }

TESTS="fixtures/eval-okay-fromtoml-rules fixtures/eval-okay-fromjson-rules
fixtures/eval-okay-toxml-rules corpus/tvix/tvix_tests/eval-okay-builtins-toString
fixtures/eval-okay-floats fixtures/eval-okay-derivation-fixed-output
corpus/tvix/nix_tests/eval-okay-hashstring fixtures/eval-okay-derivation-context
fixtures/eval-fail-derivation-hash-type fixtures/eval-fail-regex-context
fixtures/eval-okay-hashstring-algos fixtures/eval-okay-regex-rules
fixtures/eval-fail-tofile-name fixtures/eval-okay-deep-equality
corpus/tvix/nix_tests/eval-okay-tojson fixtures/eval-okay-sort
fixtures/eval-okay-gsos-read fixtures/eval-okay-derivation-chain
fixtures/eval-okay-deep-nesting"

for t in $TESTS; do
    "$NBC" "tests/$t.nix" -o "$WORK/$(basename "$t").nbc" || { echo "cannot compile $t" >&2; exit 1; }
done

# run <cvm.bin> <test> [top]: a2min's output, the stack starting at top (hex) or STACK_TOP
run() {
    A2MIN_STACK_TOP="${3:-}" A2MIN_MOUNT="/pwd/lang=$PWD/tests/$(dirname "$2")" \
        A2MIN_VAR_TEST_VAR=foo "$A2MIN" --run-nbc "$1" "$WORK/$(basename "$2").nbc" 2>&1
}

# sweep <cvm.bin> <test> [first]: 0 if every top gives the answer or E_NOMEM.
sweep() {
    local vm="$1" t="$2" first="${3:-}" top want got used start ok=0 nomem=0 bad=0
    want=$(tr -d '\n' < "tests/$t.exp")
    # With the whole stack, how deep it goes: no check is deeper than that, and
    # no stack_need() asks for more than NEED over STACK_MIN, so starting a
    # step over both puts each check at every depth near where it refuses.
    used=$(run "$vm" "$t" | sed -n 's/^  stack: lowest S \$[0-9A-F]*, \([0-9]*\) bytes under the top$/\1/p')
    [ -n "$used" ] || { echo "  $t: a2min printed no stack line"; return 1; }
    start=$((MIN + used + NEED + STEP)); [ $start -le $TOP ] || start=$TOP
    for ((top = start; top >= MIN; top -= STEP)); do
        got=$(run "$vm" "$t" "$(printf '%X' $top)" | sed -n 's/^result: //p; /^error: /p' | head -1)
        if [ "$got" = "$want" ]; then ok=$((ok + 1))
        elif [ "$got" = "error: E_NOMEM" ]; then nomem=$((nomem + 1))
        else
            bad=$((bad + 1))
            echo "  $t at S=\$$(printf '%04X' $top): ${got:0:120}"
            [ -n "$first" ] && return 1
        fi
    done
    [ "$ok" -gt 0 ] || { echo "  $t: never answered"; bad=$((bad + 1)); }
    printf '  %-56s %5d deep: %3d answered, %3d E_NOMEM, %d wrong\n' "$t" "$used" $ok $nomem $bad
    [ "$bad" -eq 0 ]
}

echo "=== stack stress: every program at every stack depth, step $STEP ==="
fails=0
for t in $TESTS; do sweep "$VM" "$t" || fails=$((fails + 1)); done
[ "$fails" -eq 0 ] || { echo "STACK STRESS FAILED: $fails program(s) overflowed or answered wrongly" >&2; exit 1; }

echo "=== self-test: a VM with the old 256-byte margin ==="
mkdir -p "$WORK/src"
cp target/vm/c/*.c target/vm/c/*.h "$WORK/src/"
sed -i -e "s/^#define STACK_MIN        0x$(printf '%X' $MIN)U /#define STACK_MIN        0x$(printf '%X' $((MIN - 0x300)))U /" \
       -e '/^_Static_assert(STACK_MIN - STACK_FLOOR == /d' "$WORK/src/vm.h"
cmp -s target/vm/c/vm.h "$WORK/src/vm.h" && { echo "  PATTERN MISS -- update this script" >&2; exit 1; }
srcs=""
for s in $(grep -v '^#' target/vm/c/SOURCES); do srcs="$srcs $WORK/src/$s"; done
# shellcheck disable=SC2086
orcabuild -o "$WORK/planted.bin" -O $srcs > "$WORK/build.log" 2>&1 || { cat "$WORK/build.log" >&2; exit 1; }
if sweep "$WORK/planted.bin" fixtures/eval-okay-derivation-fixed-output first; then
    echo "STACK STRESS SURVIVED a VM whose checks come too late" >&2; exit 1
fi
echo "STACK STRESS OK: no program overflows the C stack at any depth, and a late check is caught"
