#!/usr/bin/env bash
# Host-side lint of the VM's C (target/vm/c), in seconds and without ORCA/C:
#
#   1. clang -Wall -Wextra -Werror on every file, after scripts/hostlint/
#      orca2host.py has rewritten the two ORCA/C-only constructs (`segment`
#      and asm{}) with line numbers kept. i686 because pointers are 32 bits
#      there, as on the 65816; ORCA/C's own lint (vm.h) is what sees the
#      16-bit int.
#   2. scripts/hostlint/gcsafe.py: no obj_t * local, and no view, read after a
#      call that may collect or turn the window ring over (vm.h's "one rule").
#      The collecting set is computed from the call graph. It is a lint, not
#      a proof: the stress modes (GC periods, reserve stress) stay the
#      authority, and a deliberate exception is marked `/* gcsafe: <why> */`.
#   3. scripts/hostlint/orcabugs.py: constructs ORCA/C 2.3.0 b1 is known to
#      miscompile (TOOLCHAIN.md "Known miscompiles"), each found on a2min first.
#   4. scripts/hostlint/segments.py: every file but main.c puts its functions
#      in a `segment` of its own, so none lands in bank 0 by default (D31).
#
# Then the self-test (DECISIONS.md D14): each rule broken in a COPY, and the
# lint must refuse it. Three of the GC plants are scripts/mutation-vm.sh's,
# which needs an ORCA/C build and seven stress modes to catch each one.
#
# Usage: c-lint.sh   (from the repo root; needs clang and python3 with libclang)
set -uo pipefail

R=$PWD
H=$R/scripts/hostlint
SRCS=$(grep -v '^#' target/vm/c/SOURCES)           # link order: main.c first
RES=$(clang -print-resource-dir)
CF=(--target=i686-linux-gnu -ffreestanding -nostdlibinc -isystem "$RES/include" -isystem "$H/include"
    -std=gnu99 -Wno-unknown-pragmas -Wno-unused-command-line-argument -include "$H/orca_shim.h" -I.)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# lint DIR: 0 clean, 1 clang refused it, 2 gcsafe did, 3 orcabugs did, 4 segments did.
# Findings in $WORK/out.
lint() {
    local f rc=0
    rm -rf "$WORK/host"; mkdir -p "$WORK/host"
    for f in "$1"/*.c "$1"/*.h; do python3 -I "$H/orca2host.py" "$f" "$WORK/host/$(basename "$f")"; done
    : > "$WORK/out"
    for f in $SRCS; do
        (cd "$WORK/host" && clang -fsyntax-only -Wall -Wextra -Werror "${CF[@]}" "$f") >> "$WORK/out" 2>&1 || rc=1
    done
    [ $rc -eq 0 ] || return 1
    # shellcheck disable=SC2086
    (cd "$WORK/host" && python3 -I "$H/gcsafe.py" $SRCS -- "${CF[@]}") >> "$WORK/out" 2>&1 || return 2
    # shellcheck disable=SC2086
    (cd "$WORK/host" && python3 -I "$H/orcabugs.py" $SRCS -- "${CF[@]}") >> "$WORK/out" 2>&1 || return 3
    # shellcheck disable=SC2086
    python3 -I "$H/segments.py" "$1" $SRCS >> "$WORK/out" 2>&1 || return 4
    return 0
}

echo "=== C lint: host clang, gcsafe, orcabugs and segments ==="
lint target/vm/c
case $? in
    0) echo "  the VM is clean" ;;
    *) echo "  the VM is NOT clean:"; sed "s|$WORK/host/|target/vm/c/|" "$WORK/out"; exit 1 ;;
esac

fail=0
# plant <name> <file> <sed-expression> <expected status: 1 clang, 2 gcsafe, 3 orcabugs, 4 segments>
plant() {
    rm -rf "$WORK/src"; cp -r target/vm/c "$WORK/src"
    sed -i "$3" "$WORK/src/$2"
    if cmp -s "target/vm/c/$2" "$WORK/src/$2"; then
        printf '  %-54s PATTERN MISS -- update this script\n' "$1"; fail=1; return
    fi
    lint "$WORK/src"
    local got=$?
    if [ "$got" -eq "$4" ]; then printf '  %-54s refused\n' "$1"
    elif [ "$got" -eq 0 ]; then printf '  %-54s ACCEPTED -- the lint is decorative\n' "$1"; fail=1
    else printf '  %-54s refused by the wrong check (%d)\n' "$1" "$got"; fail=1
    fi
}

plant "clang: an unused static function" card.c '$a static void lint_plant(void) { }' 1
plant "clang: a variable set but never read" sha256.c '$a void lint_plant(void) { int planted; planted = 1; }' 1
plant "gcsafe: ++ keeps its views across a collecting reserve" run.c \
      's/            if (nursery_reserve_n(n + i, 4, 4)) view_two(\&a, \&b);/            nursery_reserve_n(n + i, 4, 4);/' 2
plant "gcsafe: map keeps its view across a collecting reserve" prims.c \
      's/    if (nursery_reserve_n(n, 32, 4)) l = view_top_as(T_LIST);/    nursery_reserve_n(n, 32, 4);/' 2
plant "gcsafe: string + reads a part it did not view" strings.c \
      '/^    o = str_alloc(len);$/,/^    STRCTX(o, len) = pop();$/ s/^        s = deref(vstack\[at + i\]);$/        s = vstack[at + i];/' 2
plant "gcsafe: removeAttrs keeps the set it forced past" prims.c \
      '/^static void prim_removeattrs(void)/,/^}/ s/^    so = rstack\[b\];$/    \/\* planted \*\//' 2
plant "gcsafe: store_field keeps its object across a collection" data.c \
      's/rpush(c); push(v); gc_collect(); v = pop(); c = rpop();/push(v); gc_collect(); v = pop();/' 2
plant "gcsafe: a collecting call inside push(), then a read" strings.c \
      's/^    case T_FLOAT:  push(float_str(v)); return;/    case T_FLOAT:  push(float_str(v));/' 2
plant "orcabugs: a cast of an unsigned 64-bit difference in ?:" float.c \
      '$a int64_t lint_plant(uint64_t m, int c) { return c ? (int64_t)m : (int64_t)(m - 1); }' 3
plant "orcabugs: a 64-bit ?: with a call in an arm" float.c \
      '$a uint64_t lint_plant(uint64_t m) { return m ? f_from_int(1) : m; }' 3
plant "segments: a file with no segment line" gsos.c '/^segment "gsos";$/d' 4
plant "segments: a function above its file's segment line" hashes.c \
      '/^segment "hashes";$/i void lint_plant(void);\nvoid lint_plant(void)\n{\n}' 4
plant "segments: the entry point's file in a segment" main.c '/^#include "vm.h"$/a segment "core";' 4

echo
[ "$fail" -eq 0 ] || exit 1
echo "C LINT OK: the VM is clean, and every plant is refused"
