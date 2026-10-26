#!/usr/bin/env bash
# Prove the compiler's own checks can FAIL (DECISIONS.md D14): ORCA/C's lint,
# which vm.h turns on for every VM file (`#pragma lint -1`), the linker's
# refusal of an unresolved reference, and the _Static_asserts that pin the
# memory map. A check that cannot fail is
# decorative, and a pragma placed where it does not apply would be exactly
# that -- so each plant below goes into a COPY of the sources (never the tree),
# and must stop the build with its own message, while the unplanted copy builds.
#
# Usage: orca-lint-selftest.sh   (from the repo root; needs orcabuild on PATH)
set -uo pipefail

SRCS=$(grep -v '^#' target/vm/c/SOURCES)           # link order: main.c first
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fresh() { rm -rf "$WORK/src"; cp -r target/vm/c "$WORK/src"; }
build() {
    local s list=""
    for s in $SRCS; do list="$list $WORK/src/$s"; done
    orcabuild -o "$WORK/cvm.bin" -O $list > "$WORK/log" 2>&1
}

echo "=== ORCA/C lint and static asserts (D14) ==="
fresh
if ! build; then
    echo "  BASELINE DID NOT BUILD" >&2
    tr -d '\\|/-' < "$WORK/log" | tail -20 >&2
    exit 1
fi
echo "  baseline builds"

fail=0
# plant <name> <file> <sed-expression> <text the build must report>
plant() {
    fresh
    sed -i "$3" "$WORK/src/$2"
    if cmp -s "target/vm/c/$2" "$WORK/src/$2"; then
        printf '  %-44s PATTERN MISS -- update this script\n' "$1"; fail=1; return
    fi
    if build; then
        printf '  %-44s BUILT -- the check is decorative\n' "$1"; fail=1
    elif grep -q "$4" "$WORK/log"; then
        printf '  %-44s refused\n' "$1"
    else
        printf '  %-44s FAILED FOR ANOTHER REASON\n' "$1"; tr -d '\\|/-' < "$WORK/log" | tail -8; fail=1
    fi
}

# The plants are appended, so they do not depend on any line of the VM. prims.c
# and sha256.c are their own load segment: the pragma in vm.h must reach them.
plant "lint: an unused variable (prims.c)"   prims.c  '$a static void lint_plant(void) { int planted; }' 'unused variable'
plant "lint: an unused variable (sha256.c)"  sha256.c '$a static void lint_plant(void) { int planted; }' 'unused variable'
plant "lint: an undefined function (run.c)"  run.c    '$a void lint_plant(void) { planted(); }'           'undefined function'
plant "lint: a constant changed (data.c)"    data.c   '$a void lint_plant(void) { uint8_t c = 300; (void)c; }' 'changes value of constant'
plant "link: an unresolved reference (data.c)" data.c '$a extern obj_t *planted_global; void lint_plant(void) { planted_global = 0; }' 'Unresolved reference'
plant "assert: the remembered set outgrows bank 5" vm.h 's/^#define RSET_N           256$/#define RSET_N           512/' 'fits in bank 5'

echo
[ "$fail" -eq 0 ] || exit 1
echo "ORCA LINT OK: every plant is refused, and the VM builds clean"
