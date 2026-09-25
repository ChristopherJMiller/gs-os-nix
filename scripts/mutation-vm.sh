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
#   mask:M    a normal run with verbs M on the CPU (A2MIN_VERB_MASK): 4 puts
#             SHA-256 on sha256.c, a path no other mode reaches
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
# The derivation fixtures first: they are cheap, and run_suite stops at the
# first wrong answer, so a mutation they catch costs seconds. The refusal is a
# fixture too -- its .exp is the error line (scripts/scoreboard.py).
FIXTURES="eval-okay-derivation eval-okay-derivation-escapes eval-okay-derivation-nulls eval-okay-derivation-equality eval-okay-tostring-lists refuse-derivation-dependent eval-okay-remembered-set eval-okay-deep-equality eval-okay-big-operations eval-okay-paths eval-okay-coerce-sets eval-okay-hashstring"
MODES="limit:0 limit:1 period:2 period:3 both:4 reserve:1 mask:4"
SRCS="main.c run.c data.c prims.c gc.c card.c sha256.c"   # link order: main.c first
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
                                --run-nbc "$WORK/cvm.bin" "$WORK/$t.nbc" 2>&1 | sed -n 's/^result: //p; /^error: /p') ;;
                period:*) got=$(A2MIN_GC_PERIOD=${mode#*:} A2MIN_BUDGET=$BUDGET "$A2MIN" \
                                --run-nbc-gc-stress "$WORK/cvm.bin" "$WORK/$t.nbc" 2>&1 | sed -n 's/^result: //p; /^error: /p') ;;
                both:*)   got=$(A2MIN_GC_PERIOD=${mode#*:} A2MIN_RSET_LIMIT=1 A2MIN_BUDGET=$BUDGET "$A2MIN" \
                                --run-nbc-gc-stress "$WORK/cvm.bin" "$WORK/$t.nbc" 2>&1 | sed -n 's/^result: //p; /^error: /p') ;;
                reserve:*) got=$(A2MIN_RESERVE_STRESS=${mode#*:} A2MIN_BUDGET=$BUDGET "$A2MIN" \
                                --run-nbc "$WORK/cvm.bin" "$WORK/$t.nbc" 2>&1 | sed -n 's/^result: //p; /^error: /p') ;;
                mask:*)   got=$(A2MIN_VERB_MASK=${mode#*:} A2MIN_BUDGET=$BUDGET "$A2MIN" \
                                --run-nbc "$WORK/cvm.bin" "$WORK/$t.nbc" 2>&1 | sed -n 's/^result: //p; /^error: /p') ;;
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

# Paths (BYTECODE.md "Constants"): a PATH has no context word, so the collector
# must size it itself -- the 8-byte default truncates any path longer than 4
# bytes when it is promoted -- and `+` must canonicalise as Nix does.
mutate "a PATH is sized by the 8-byte default" gc.c \
       's/^    case T_PATH:   return n ? round4(n) + 4 : 8;    \/\* no context word (HEAP.md) \*\///'

# String coercion (prims.c coerce_top): "${drv}" is an outPath coercion, and
# concatStringsSep must copy each element's COERCED text -- its first version
# copied the set itself and printed its header bytes.
mutate "outPath does not coerce" prims.c \
       's/^        } else if (find_attr(vo, str_outpath, \&ix, \&f)) {$/        } else if (0) {/'

mutate "concatStringsSep copies the set, not its coercion" prims.c \
       's/^        if (coerce) e = deref(vstack\[at + i\]);$/        if (0) e = deref(vstack[at + i]);/'

# SHA-256 (prims.c prim_hashstring): three paths, each reached by some mode --
# HASH_STREAM only for a mature string (the period modes), sha256.c only with
# the verb masked (mask:4), the digest readout on every card run.
mutate "HASH_STREAM starts a byte late" prims.c \
       's/        if (!card_sha256_at(PBITS(so) + 4, n, dig))/        if (!card_sha256_at(PBITS(so) + 5, n, dig))/'

mutate "SHA-256 on the CPU pads the wrong length" sha256.c \
       's/    uint32_t bits = (uint32_t)n << 3; /    uint32_t bits = (uint32_t)n << 2; /'

mutate "the digest is read from the wrong slot" vm.h \
       's/^#define card_sha256(s, n, o)    card_hash((s), (n), (o), 0)$/#define card_sha256(s, n, o)    card_hash((s), (n), (o), 3)/'

mutate "path + keeps .. components" data.c \
       's/        if (len == 2 \&\& PCH(start) == '"'"'.'"'"' \&\& PCH(start + 1) == '"'"'.'"'"') {/        if (0) {/'

# Derivations (docs/DERIVATION.md): each rule the host was measured to follow,
# broken. Every one of these gives a well-formed store path -- only the host's
# own answer in the .exp tells it is the wrong one.
mutate "system is escaped like the builder" prims.c \
       's/^    put_str(k, PAIR_VAL(d, d->sys), 0); /    put_str(k, PAIR_VAL(d, d->sys), 1); /'

mutate "a tab is not escaped" prims.c \
       's/ || c == .\\t.) {$/) {/'

mutate "an output does not replace an attribute of its name" prims.c \
       's/^            if (c == 1) ++i;$//'

mutate "__contentAddressed = false goes into the environment" prims.c \
       's/^    if (IS_LIT(nm, "__contentAddressed")) return K_CA;$//'

mutate "a second output's path has no -<output> suffix" prims.c \
       's/ol = IS_LIT(ov, "out") ? 0 : AUX16(ov);/ol = 0;/'

mutate "the .drv is hashed from the masked ATerm" prims.c \
       's/^    d.masked = 0;$//'

mutate "__ignoreNulls keeps nulls" prims.c \
       's/            if (deref(peek())->tag == T_NULL) { vsp -= 2; continue; }/            if (0) { vsp -= 2; continue; }/'

mutate "toString puts a space after an empty list" prims.c \
       's/        space = i + 1 < n \&\& !(v->tag == T_LIST \&\& AUX16(v) == 0);/        space = i + 1 < n;/'

mutate "a store path is read from P0, the digest" vm.h \
       's/^#define card_storepath(s, n, o) card_hash((s), (n), (o), 3)$/#define card_storepath(s, n, o) card_hash((s), (n), (o), 0)/'

mutate "the CPU does not fold the store-path hash" sha256.c \
       's/^    for (i = 20; i < 32; ++i) f\[i - 20\] ^= dig\[i\];$//'

mutate "two derivations compare as sets" prims.c \
       's/    if (is_drv(rstack\[base\]) \&\& is_drv(rstack\[base + 1\]) \&\&/    if (0 \&\&/'

mutate "a second derivation is not refused" prims.c \
       's/^    if (drv_made) err_exit(E_UNSUP);$//'

echo
echo "$pass mutations caught, $fail not caught"
[ "$fail" -eq 0 ] || exit 1
echo "MUTATION PASS: every broken collector and derivation rule is caught by the fixtures"
