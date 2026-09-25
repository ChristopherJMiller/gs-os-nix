#!/usr/bin/env bash
# Prove the native-mode self-test can actually FAIL.
#
# sim/cpu65816.h is the 65816 the L4 VM runs on -- and, through orcarun, the one
# ORCA/C itself runs on. If its model is wrong, every VM result is suspect -- and
# target/tests/native_test.s only guards against that if it can tell a right
# interpreter from a wrong one. So this breaks the interpreter in specific ways,
# one at a time, and asserts that `a2min --selftest` notices each one. A mutation
# that survives means the corresponding case is decorative and must be fixed (see
# DECISIONS.md D14 for why this matters).
#
# The whole card is Verilated once; each mutation then rebuilds only a2min.o via the
# generated makefile, so the loop is seconds per mutation rather than minutes.
#
# The mutations are applied to a COPY of rtl/ and sim/ in a temporary directory,
# never to the tree. This script used to edit sim/cpu65816.h in place and restore
# it from a trap -- but a SIGKILL (the low-memory reaper) skips every trap, which
# is how scripts/mutation-vm.sh twice left a mutation behind in target/vm/c.
set -uo pipefail

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cp -r rtl sim "$WORK/"
FILE="$WORK/sim/cpu65816.h"
HARNESS="$WORK/sim/a2min.cpp"
ORIG="$WORK/cpu65816.h.orig"
cp "$FILE" "$ORIG"
BIN=target/build/native_test.bin
DIR="$WORK/build"
JOBS="${NIX_BUILD_CORES:-0}"

restore() { cp "$ORIG" "$FILE"; }

pass=0
fail=0

if [ ! -f "$BIN" ]; then
    mkdir -p target/build
    ca65 --cpu 65816 -o target/build/native_test.o target/tests/native_test.s
    ld65 -C target/tests/apple2.cfg -o "$BIN" target/build/native_test.o
fi

# Build the harness. Returns 2 if it does not compile. The generated makefile does
# not track the header, so the harness source is touched to force a recompile.
build() {
    if [ ! -f "$DIR/Va2nix_card.mk" ]; then
        rm -rf "$DIR"
        ( cd "$WORK" && verilator --cc --exe --build -j "$JOBS" -Irtl --top-module a2nix_card \
            -Wall -Wno-DECLFILENAME \
            --Mdir build -o a2min \
            rtl/a2nix_card.v sim/a2min.cpp ) > /dev/null 2>&1 || return 2
    else
        touch "$FILE" "$HARNESS"
        make -s -C "$DIR" -f Va2nix_card.mk > /dev/null 2>&1 || return 2
    fi
}

# Run the self-test against the current interpreter. 0 = it passed.
run_suite() {
    build || return 2
    "$DIR"/a2min --selftest "$BIN" > /dev/null 2>&1
}

# mutate <name> <sed-expression>
#   Applies the mutation to sim/cpu65816.h, expects the self-test to FAIL, reverts.
#
# A pattern that does not match is a FAILURE, not a skip -- an inert mutation would
# report "caught" forever while testing nothing. A mutation that does not compile is
# also a failure: it would be "caught" for the wrong reason.
mutate() {
    local name="$1" expr="$2"
    restore
    sed -i "$expr" "$FILE"

    if cmp -s "$ORIG" "$FILE"; then
        printf '  %-48s PATTERN MISS -- update this script\n' "$name"
        fail=$((fail + 1))
        restore
        return
    fi

    run_suite
    local rc=$?
    restore

    case $rc in
        0) printf '  %-48s SURVIVED -- the case is decorative!\n' "$name"
           fail=$((fail + 1)) ;;
        2) printf '  %-48s DID NOT BUILD -- fix the mutation\n' "$name"
           fail=$((fail + 1)) ;;
        *) printf '  %-48s caught (rc=%d)\n' "$name" "$rc"
           pass=$((pass + 1)) ;;
    esac
}

echo "=== mutation testing the native-mode self-test ==="
echo "Baseline: the self-test must PASS on the unmutated interpreter."
run_suite
if [ $? -ne 0 ]; then
    echo "  BASELINE FAILED -- fix a2min before mutation testing." >&2
    exit 1
fi
echo "  baseline passes"
echo
echo "Each mutation breaks one 65816 feature; the self-test must catch it."

mutate "(sr,S),Y ignores Y" \
       's/sp + fetch8()))) + y) \& 0xFFFFFF;/sp + fetch8()))) + 0) \& 0xFFFFFF;/'

mutate "sr,S reads from the direct page" \
       's/uint32_t ea_sr()        { return static_cast<uint16_t>(sp + fetch8()); }/uint32_t ea_sr()        { return static_cast<uint16_t>(dp + fetch8()); }/'

mutate "[dp] ignores the bank byte" \
       's/ea_dp_long()   { return rd24(/ea_dp_long()   { return rd16(/'

mutate "[dp],Y ignores Y" \
       's/ea_dp_long_y() { return (rd24(static_cast<uint16_t>(dp + fetch8())) + y)/ea_dp_long_y() { return (rd24(static_cast<uint16_t>(dp + fetch8())) + 0)/'

mutate "long,X ignores X" \
       's/ea_long_x()    { return (fetch24() + x)/ea_long_x()    { return (fetch24() + 0)/'

mutate "absolute addressing ignores DBR" \
       's/uint32_t dbr_(uint16_t off) const { return bank(dbr, off); }/uint32_t dbr_(uint16_t off) const { return bank(0, off); }/'

mutate "JSL does not push the program bank" \
       's/push8(pbr); push16(static_cast<uint16_t>(pc - 1));/push16(static_cast<uint16_t>(pc - 1));/'

mutate "RTL does not restore the program bank" \
       's/pc = static_cast<uint16_t>(pop16() + 1); pbr = pop8(); cyc = 6;/pc = static_cast<uint16_t>(pop16() + 1); pop8(); cyc = 6;/'

mutate "JML stays in the current bank" \
       's/case 0x5C: { const uint32_t t = fetch24(); pbr = t >> 16;/case 0x5C: { const uint32_t t = fetch24(); pbr = pbr;/'

mutate "16-bit ROR rotates the carry into bit 7" \
       's/(v_ >> 1) | (cin ? signA() : 0)/(v_ >> 1) | (cin ? 0x80 : 0)/'

mutate "SEP #\$20 clears B" \
       's/if (m8()) a = static_cast<uint16_t>((a \& 0xFF00) | (v_ \& 0xFF)); else a = v_;/if (m8()) a = static_cast<uint16_t>(v_ \& 0xFF); else a = v_;/'

mutate "XCE keeps 16-bit index registers" \
       's/            if (e) { x \&= 0xFF; y \&= 0xFF; fix_sp(); }/            if (e) { fix_sp(); }/'

mutate "REP cannot widen the index registers" \
       's/            xf = p \& 0x10;/            xf = true;/'

mutate "ADC carries out of bit 8 in 16-bit mode" \
       's/        c = s > maskA();/        c = s > 0xFF;/'

mutate "CMP compares the low byte only" \
       's/        r \&= mask; mval \&= mask;/        r \&= 0xFF; mval \&= 0xFF;/'

mutate "TCD is a no-op" \
       's/case 0x5B: dp = a;/case 0x5B: dp = dp;/'

mutate "16-bit PLA pops a single byte" \
       's/case 0x68: { const uint16_t v_ = m8() ? pop8() : pop16();/case 0x68: { const uint16_t v_ = pop8();/'

mutate "MVN copies downward" \
       's/const int dir = (op == 0x54) ? 1 : -1;/const int dir = (op == 0x54) ? -1 : 1;/'

mutate "TSB abs leaves memory unchanged" \
       's/wrA(ea, (mv | getA()) \& maskA());  cyc += 4/wrA(ea, mv);  cyc += 4/'

mutate "TRB abs leaves memory unchanged" \
       's/wrA(ea, (mv \& ~getA()) \& maskA()); cyc += 4/wrA(ea, mv); cyc += 4/'

echo
echo "$pass mutations caught, $fail not caught"
[ "$fail" -eq 0 ] || exit 1
echo "MUTATION PASS: every broken 65816 feature is caught by the self-test"
