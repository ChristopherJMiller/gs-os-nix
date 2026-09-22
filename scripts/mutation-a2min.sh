#!/usr/bin/env bash
# Prove the native-mode self-test can actually FAIL.
#
# sim/a2min.cpp is the machine the L4 VM will be developed on. If its 65816 model is
# wrong, every VM result is suspect -- and target/tests/native_test.s only guards
# against that if it can tell a right interpreter from a wrong one. So this breaks
# the interpreter in specific ways, one at a time, and asserts that `--selftest`
# notices each one. A mutation that survives means the corresponding case is
# decorative and must be fixed (see DECISIONS.md D14 for why this matters).
#
# The whole card is Verilated once; each mutation then rebuilds only a2min.o via the
# generated makefile, so the loop is seconds per mutation rather than minutes.
#
# Every mutation is reverted afterwards, including on failure or interrupt.
set -uo pipefail

FILE=sim/a2min.cpp
BIN=target/build/native_test.bin
DIR=sim/build_mutation_a2min
JOBS="${NIX_BUILD_CORES:-0}"

BACKUP=$(mktemp)
cp "$FILE" "$BACKUP"
restore() { cp "$BACKUP" "$FILE"; }
trap 'restore; rm -f "$BACKUP"' EXIT INT TERM

pass=0
fail=0

if [ ! -f "$BIN" ]; then
    mkdir -p target/build
    ca65 --cpu 65816 -o target/build/native_test.o target/tests/native_test.s
    ld65 -C target/tests/apple2.cfg -o "$BIN" target/build/native_test.o
fi

# Build the harness. Returns 2 if it does not compile.
build() {
    if [ ! -f "$DIR/Va2nix_card.mk" ]; then
        rm -rf "$DIR"
        verilator --cc --exe --build -j "$JOBS" -Irtl --top-module a2nix_card \
            -Wall -Wno-DECLFILENAME \
            --Mdir "$DIR" -o a2min \
            rtl/a2nix_card.v "$FILE" > /dev/null 2>&1 || return 2
    else
        touch "$FILE"
        make -s -C "$DIR" -f Va2nix_card.mk > /dev/null 2>&1 || return 2
    fi
}

# Run the self-test against the current interpreter. 0 = it passed.
run_suite() {
    build || return 2
    ./"$DIR"/a2min --selftest "$BIN" > /dev/null 2>&1
}

# mutate <name> <sed-expression>
#   Applies the mutation to sim/a2min.cpp, expects the self-test to FAIL, reverts.
#
# A pattern that does not match is a FAILURE, not a skip -- an inert mutation would
# report "caught" forever while testing nothing. A mutation that does not compile is
# also a failure: it would be "caught" for the wrong reason.
mutate() {
    local name="$1" expr="$2"
    restore
    sed -i "$expr" "$FILE"

    if cmp -s "$BACKUP" "$FILE"; then
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
       's/cpu.sp + fetch8()))) + cpu.y) \& 0xFFFFFF;/cpu.sp + fetch8()))) + 0) \& 0xFFFFFF;/'

mutate "sr,S reads from the direct page" \
       's/uint32_t ea_sr()        { return static_cast<uint16_t>(cpu.sp + fetch8()); }/uint32_t ea_sr()        { return static_cast<uint16_t>(cpu.dp + fetch8()); }/'

mutate "[dp] ignores the bank byte" \
       's/ea_dp_long()   { return rd24(/ea_dp_long()   { return rd16(/'

mutate "[dp],Y ignores Y" \
       's/ea_dp_long_y() { return (rd24(static_cast<uint16_t>(cpu.dp + fetch8())) + cpu.y)/ea_dp_long_y() { return (rd24(static_cast<uint16_t>(cpu.dp + fetch8())) + 0)/'

mutate "long,X ignores X" \
       's/ea_long_x()    { return (fetch24() + cpu.x)/ea_long_x()    { return (fetch24() + 0)/'

mutate "absolute addressing ignores DBR" \
       's/uint32_t dbr_(uint16_t off) { return bank(cpu.dbr, off); }/uint32_t dbr_(uint16_t off) { return bank(0, off); }/'

mutate "JSL does not push the program bank" \
       's/                 push8(cpu.pbr); push16(static_cast<uint16_t>(cpu.pc - 1));/                 push16(static_cast<uint16_t>(cpu.pc - 1));/'

mutate "RTL does not restore the program bank" \
       's/cpu.pc = static_cast<uint16_t>(pop16() + 1); cpu.pbr = pop8(); cyc = 6;/cpu.pc = static_cast<uint16_t>(pop16() + 1); pop8(); cyc = 6;/'

mutate "JML stays in the current bank" \
       's/case 0x5C: { const uint32_t t = fetch24(); cpu.pbr = t >> 16;/case 0x5C: { const uint32_t t = fetch24(); cpu.pbr = cpu.pbr;/'

mutate "16-bit ROR rotates the carry into bit 7" \
       's/(v >> 1) | (cin ? signA() : 0)/(v >> 1) | (cin ? 0x80 : 0)/'

mutate "SEP #\$20 clears B" \
       's/if (m8()) cpu.a = static_cast<uint16_t>((cpu.a \& 0xFF00) | (v \& 0xFF)); else cpu.a = v;/if (m8()) cpu.a = static_cast<uint16_t>(v \& 0xFF); else cpu.a = v;/'

mutate "XCE keeps 16-bit index registers" \
       's/        if (cpu.e) { cpu.x \&= 0xFF; cpu.y \&= 0xFF; fix_sp(); }/        if (cpu.e) { fix_sp(); }/'

mutate "REP cannot widen the index registers" \
       's/        cpu.xf = p \& 0x10;/        cpu.xf = true;/'

mutate "ADC carries out of bit 8 in 16-bit mode" \
       's/    cpu.c = s > maskA();/    cpu.c = s > 0xFF;/'

mutate "CMP compares the low byte only" \
       's/    r \&= mask; mval \&= mask;/    r \&= 0xFF; mval \&= 0xFF;/'

mutate "TCD is a no-op" \
       's/case 0x5B: cpu.dp = cpu.a;/case 0x5B: cpu.dp = cpu.dp;/'

mutate "16-bit PLA pops a single byte" \
       's/case 0x68: { const uint16_t v = m8() ? pop8() : pop16();/case 0x68: { const uint16_t v = pop8();/'

mutate "MVN copies downward" \
       's/const int dir = (op == 0x54) ? 1 : -1;/const int dir = (op == 0x54) ? -1 : 1;/'

echo
echo "$pass mutations caught, $fail not caught"
[ "$fail" -eq 0 ] || exit 1
echo "MUTATION PASS: every broken 65816 feature is caught by the self-test"
