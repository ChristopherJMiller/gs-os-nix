#!/usr/bin/env bash
# The C stack's checks are never too late (docs/VM.md "The C stack").
#
# The VM's stack is $0400-$1FFF, and under it are the harness's RESULT, DIAG,
# flags and STATS. stack_check() and stack_need() refuse (E_NOMEM) before the
# stack can reach them -- but only if every path between two checks fits the
# margin each leaves. A path that did not once wrote over the flags (turning
# GC stress and verb masks on mid-run) and STATS before any check saw it.
#
# So: each program below runs with its stack starting at every step from $1FFF
# down to $0800 (a2min A2MIN_STACK_TOP), which puts each of its operations at
# every depth near the floor, under a2min's tripwire (any S or write under the
# VM's stack is a loud failure). Every run must give the program's answer or
# E_NOMEM, nothing else. The programs are the ones that go deepest between
# two checks, one per kind of deep path, as measured (VM.md).
#
# Then it proves it can fail (D14): a VM built from a copy with the old margin
# (STACK_MIN $0500) must be caught.
#
# Usage: stack-stress.sh <a2min> <nbc> <cvm.bin>   (orcabuild on PATH for the self-test)
set -uo pipefail

A2MIN="$1"; NBC="$2"; VM="$3"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
STEP=${STACK_STEP:-64}

TESTS="fixtures/eval-okay-fromtoml-rules fixtures/eval-okay-fromjson-rules
fixtures/eval-okay-toxml-rules corpus/tvix/tvix_tests/eval-okay-builtins-toString
fixtures/eval-okay-floats fixtures/eval-okay-derivation-fixed-output
corpus/tvix/nix_tests/eval-okay-hashstring fixtures/eval-okay-derivation-context
fixtures/eval-fail-derivation-hash-type fixtures/eval-fail-regex-context
fixtures/eval-okay-hashstring-algos fixtures/eval-okay-regex-rules
fixtures/eval-fail-tofile-name fixtures/eval-okay-deep-equality
corpus/tvix/nix_tests/eval-okay-tojson fixtures/eval-okay-sort
fixtures/eval-okay-gsos-read fixtures/eval-okay-derivation-chain"

for t in $TESTS; do
    "$NBC" "tests/$t.nix" -o "$WORK/$(basename "$t").nbc" || { echo "cannot compile $t" >&2; exit 1; }
done

# sweep <cvm.bin> <test> [first]: 0 if every top gives the answer or E_NOMEM.
sweep() {
    local vm="$1" t="$2" first="${3:-}" top want got ok=0 nomem=0 bad=0
    want=$(tr -d '\n' < "tests/$t.exp")
    for ((top = 0x1FFF; top >= 0x0800; top -= STEP)); do
        got=$(A2MIN_STACK_TOP=$(printf '%X' $top) A2MIN_MOUNT="/pwd/lang=$PWD/tests/$(dirname "$t")" \
              A2MIN_VAR_TEST_VAR=foo "$A2MIN" --run-nbc "$vm" "$WORK/$(basename "$t").nbc" 2>&1 |
              sed -n 's/^result: //p; /^error: /p' | head -1)
        if [ "$got" = "$want" ]; then ok=$((ok + 1))
        elif [ "$got" = "error: E_NOMEM" ]; then nomem=$((nomem + 1))
        else
            bad=$((bad + 1))
            echo "  $t at S=\$$(printf '%04X' $top): ${got:0:120}"
            [ -n "$first" ] && return 1
        fi
    done
    [ "$ok" -gt 0 ] || { echo "  $t: never answered"; bad=$((bad + 1)); }
    printf '  %-56s %3d answered, %3d E_NOMEM, %d wrong\n' "$t" $ok $nomem $bad
    [ "$bad" -eq 0 ]
}

echo "=== stack stress: every program at every stack depth, step $STEP ==="
fails=0
for t in $TESTS; do sweep "$VM" "$t" || fails=$((fails + 1)); done
[ "$fails" -eq 0 ] || { echo "STACK STRESS FAILED: $fails program(s) overflowed or answered wrongly" >&2; exit 1; }

echo "=== self-test: a VM with the old 256-byte margin ==="
mkdir -p "$WORK/src"
cp target/vm/c/*.c target/vm/c/*.h "$WORK/src/"
sed -i 's/^#define STACK_MIN        0x0800U /#define STACK_MIN        0x0500U /' "$WORK/src/vm.h"
cmp -s target/vm/c/vm.h "$WORK/src/vm.h" && { echo "  PATTERN MISS -- update this script" >&2; exit 1; }
srcs=""
for s in $(grep -v '^#' target/vm/c/SOURCES); do srcs="$srcs $WORK/src/$s"; done
# shellcheck disable=SC2086
orcabuild -o "$WORK/planted.bin" -O $srcs > "$WORK/build.log" 2>&1 || { cat "$WORK/build.log" >&2; exit 1; }
if sweep "$WORK/planted.bin" fixtures/eval-okay-derivation-fixed-output first; then
    echo "STACK STRESS SURVIVED a VM whose checks come too late" >&2; exit 1
fi
echo "STACK STRESS OK: no program overflows the C stack at any depth, and a late check is caught"
