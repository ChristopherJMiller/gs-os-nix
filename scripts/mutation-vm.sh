#!/usr/bin/env bash
# Prove the VM's collector tests can FAIL (DECISIONS.md D14, D20).
#
# Every way of getting the remembered set wrong is silent: a field that is not
# recorded, not evacuated or not written back leaves a card object holding a
# stale nursery address, which reads back as some other object -- a plausible
# wrong answer. The same is true of an object too small to hold its forwarding
# pointer. So this breaks the VM in specific ways, one at a time, rebuilds it
# with ORCA/C, and requires the fixtures below to notice in at least one mode:
#
#   limit:0   a normal run
#   limit:1   the remembered set shrunk to one entry, so it fills
#   period:K  GC stress at every K-th boundary, which puts collections between
#             a `let`'s ENV_NEW and its ENV_SETs, and at every phase of a
#             program. Not stress at EVERY boundary: that promotes each value
#             before it can be stored, so the set is never used.
#   reserve:1 every nursery_reserve() collects (A2MIN_RESERVE_STRESS): each is a
#             mid-instruction collection, so a site that keeps a view or a
#             local across one reads a forwarded object
#   both:K    period:K with a one-entry set. The only mode in which ENV_SET
#             finds the set full: without it, "store_field ignores a full set"
#             survived, because only the period modes reach store_field with a
#             young value and only limit:1 fills the set. K = 4 because it
#             takes two young stores between collections: measured on the
#             mutated VM, K = 2 and 3 miss it and every K from 4 to 9 catches it.
#
# MUTATION_ONLY=<substring> runs just the mutations whose name contains it.
#
# The mutations are applied to a COPY of the sources in a temporary directory,
# never to the tree. The first version edited the tree in place and restored it
# from a trap; a SIGKILL (the low-memory reaper) skips every trap, and twice
# left a mutation behind in target/vm/c.
#
# Usage: mutation-vm.sh <a2min> <nbc>
# Needs orcabuild on PATH (the devShell, or the flake check's inputs).
set -uo pipefail

A2MIN="$1"
NBC="$2"
FIXTURES="eval-okay-remembered-set eval-okay-deep-equality eval-okay-big-operations"
MODES="limit:0 limit:1 period:2 period:3 both:4 reserve:1"
SRCS="main.c run.c data.c prims.c gc.c card.c"      # link order: main.c first
WORK=$(mktemp -d)
BUDGET=500000000
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/orig" "$WORK/src"
for f in $SRCS opcodes.h vm.h; do cp "target/vm/c/$f" "$WORK/orig/"; done
for t in $FIXTURES; do
    "$NBC" "tests/fixtures/$t.nix" -o "$WORK/$t.nbc" || { echo "cannot compile $t" >&2; exit 1; }
done

# Build the VM from $WORK/src and run every fixture in every mode. 0 = right
# answers everywhere, 1 = something was wrong (or stopped), 2 = did not build.
run_suite() {
    local s list=""
    for s in $SRCS; do list="$list $WORK/src/$s"; done
    orcabuild -o "$WORK/cvm.bin" -O $list > "$WORK/build.log" 2>&1 || return 2
    local t mode got want
    for t in $FIXTURES; do
        want=$(tr -d '\n' < "tests/fixtures/$t.exp")
        for mode in $MODES; do
            case $mode in
                limit:*)  got=$(A2MIN_RSET_LIMIT=${mode#*:} A2MIN_BUDGET=$BUDGET "$A2MIN" \
                                --run-nbc "$WORK/cvm.bin" "$WORK/$t.nbc" 2>&1 | sed -n 's/^result: //p') ;;
                period:*) got=$(A2MIN_GC_PERIOD=${mode#*:} A2MIN_BUDGET=$BUDGET "$A2MIN" \
                                --run-nbc-gc-stress "$WORK/cvm.bin" "$WORK/$t.nbc" 2>&1 | sed -n 's/^result: //p') ;;
                both:*)   got=$(A2MIN_GC_PERIOD=${mode#*:} A2MIN_RSET_LIMIT=1 A2MIN_BUDGET=$BUDGET "$A2MIN" \
                                --run-nbc-gc-stress "$WORK/cvm.bin" "$WORK/$t.nbc" 2>&1 | sed -n 's/^result: //p') ;;
                reserve:*) got=$(A2MIN_RESERVE_STRESS=${mode#*:} A2MIN_BUDGET=$BUDGET "$A2MIN" \
                                --run-nbc "$WORK/cvm.bin" "$WORK/$t.nbc" 2>&1 | sed -n 's/^result: //p') ;;
            esac
            [ "$got" = "$want" ] || return 1
        done
    done
    return 0
}

fresh() { rm -rf "$WORK/src"; cp -r "$WORK/orig" "$WORK/src"; }

pass=0
fail=0

# mutate <name> <file> <sed-expression>
# A pattern that does not match is a FAILURE, not a skip: an inert mutation
# would report "caught" forever while testing nothing. One that does not build
# is a failure too -- it would be caught for the wrong reason.
mutate() {
    local name="$1" file="$2" expr="$3"
    case "$name" in *"${MUTATION_ONLY:-}"*) ;; *) return ;; esac
    fresh
    sed -i "$expr" "$WORK/src/$file"
    if cmp -s "$WORK/orig/$file" "$WORK/src/$file"; then
        printf '  %-52s PATTERN MISS -- update this script\n' "$name"
        fail=$((fail + 1)); return
    fi
    run_suite
    case $? in
        0) printf '  %-52s SURVIVED -- the test is decorative!\n' "$name"; fail=$((fail + 1)) ;;
        2) printf '  %-52s DID NOT BUILD -- fix the mutation\n' "$name"; fail=$((fail + 1)) ;;
        *) printf '  %-52s caught\n' "$name"; pass=$((pass + 1)) ;;
    esac
}

echo "=== mutation testing the collector (remembered set, object sizes, reserves) ==="
echo "Baseline: the fixtures must pass in every mode on the unmutated VM."
fresh
run_suite
case $? in
    0) echo "  baseline passes" ;;
    2) echo "  BASELINE DID NOT BUILD" >&2; cat "$WORK/build.log" >&2; exit 1 ;;
    *) echo "  BASELINE FAILED -- fix the VM before mutation testing." >&2; exit 1 ;;
esac
echo

mutate "a store into a mature field is not recorded" data.c \
       's/    if (IS_YOUNG(v)) remember(c, off);//'

mutate "a mature thunk's update is not recorded" run.c \
       's/        if (IS_YOUNG(y)) remember(th, yoff);//'

mutate "a recorded field is not evacuated" gc.c \
       's/        e->val = IS_YOUNG(p) ? evac(p) : 0;/        e->val = 0;/'

mutate "a recorded field is not written back" gc.c \
       's/        card_write_at(PBITS(e->obj) + e->off, \&t, 4);/        (void)t;/'

mutate "store_field ignores a full set" data.c \
       's/    if (IS_YOUNG(v) \&\& rs_full()) {/    if (0) {/'

mutate "a thunk update ignores a full set" run.c \
       's/        if (IS_YOUNG(y) \&\& rs_full()) {/        if (0) {/'

mutate "an empty list is 4 bytes" data.c \
       's/alloc(n ? 4 + 4 \* n : 8)/alloc(4 + 4 * n)/'

mutate "an empty set is 4 bytes" data.c \
       's/alloc(n ? 4 + 12 \* n : 8)/alloc(4 + 12 * n)/'

# nursery_reserve (vm.h): an operation that allocates in proportion to its
# input collects first if it must. Without the reserve, genList 300 is E_NOMEM
# whenever it starts with less than 13 KB free...
mutate "genList does not reserve" prims.c \
       's/^    nursery_reserve_n(n, 44, 4);                \/\* f is still on the stack: a root \*\///'

# ...and a site that reserves but keeps its view across a collection reads a
# forwarded young operand: the right length, the wrong contents.
mutate "++ keeps its views across a collecting reserve" run.c \
       's/            if (nursery_reserve_n(n + i, 4, 4)) view_two(\&a, \&b);/            nursery_reserve_n(n + i, 4, 4);/'

mutate "string + keeps its views across a collecting reserve" run.c \
       's/            if (nursery_reserve_n(1, round4(AUX16(a) + AUX16(b)), 8)) view_two(\&a, \&b);/            nursery_reserve_n(1, round4(AUX16(a) + AUX16(b)), 8);/'

mutate "map keeps its view across a collecting reserve" prims.c \
       's/    if (nursery_reserve_n(n, 32, 4)) l = view_top_as(T_LIST);/    nursery_reserve_n(n, 32, 4);/'

echo
echo "$pass mutations caught, $fail not caught"
[ "$fail" -eq 0 ] || exit 1
echo "MUTATION PASS: every broken collector is caught by the fixtures"
