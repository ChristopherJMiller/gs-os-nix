#!/usr/bin/env bash
# Prove the regression suite can actually FAIL.
#
# A regression test that cannot distinguish a working card from a broken one is worse
# than no test: it produces confidence without evidence. So this reintroduces each
# fixed bug into the RTL, one at a time, and asserts that sim/regress_main.cpp catches
# it. If a mutation survives, the corresponding test is decorative and must be fixed.
#
# Every mutation is reverted afterwards, including on failure or interrupt.
set -uo pipefail

FILES=(rtl/a2nix_card.v rtl/sha256.v rtl/sha256_core.v rtl/storepath.v rtl/keycam.v)
BACKUP_DIR=$(mktemp -d)
restore() { for f in "${FILES[@]}"; do cp "$BACKUP_DIR/$(basename "$f")" "$f"; done; }
trap 'restore; rm -rf "$BACKUP_DIR"' EXIT INT TERM
for f in "${FILES[@]}"; do cp "$f" "$BACKUP_DIR/$(basename "$f")"; done

DIR=sim/build_mutation
pass=0
fail=0

# Run the suite against the current RTL. Returns 0 if it passes.
run_suite() {
    rm -rf "$DIR"
    verilator --cc --exe --build -j 0 -Irtl --top-module a2nix_card \
        -Wall -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL -Wno-UNUSEDPARAM \
        --Mdir "$DIR" -o regress \
        rtl/a2nix_card.v sim/regress_main.cpp > /dev/null 2>&1 || return 2
    ./"$DIR"/regress > /dev/null 2>&1
}

# mutate <name> <file> <sed-expression>
#   Applies the mutation, expects the suite to FAIL, then reverts.
#
# A pattern that does not match counts as a FAILURE, not a skip: a silently inert
# mutation would report "caught" forever while testing nothing.
mutate() {
    local name="$1" file="$2" expr="$3"
    restore
    sed -i "$expr" "$file"

    if cmp -s "$BACKUP_DIR/$(basename "$file")" "$file"; then
        printf '  %-46s PATTERN MISS -- update this script\n' "$name"
        fail=$((fail + 1))
        restore
        return
    fi

    run_suite
    local rc=$?
    restore

    case $rc in
        0) printf '  %-46s SURVIVED -- the test is decorative!\n' "$name"
           fail=$((fail + 1)) ;;
        1) printf '  %-46s caught\n' "$name"
           pass=$((pass + 1)) ;;
        2) printf '  %-46s caught (did not build)\n' "$name"
           pass=$((pass + 1)) ;;
        *) printf '  %-46s unexpected rc=%d\n' "$name" "$rc"
           fail=$((fail + 1)) ;;
    esac
}

echo "=== mutation testing the regression suite ==="
echo "Baseline: the suite must PASS on unmutated RTL."
run_suite
if [ $? -ne 0 ]; then
    echo "  BASELINE FAILED -- fix the card before mutation testing." >&2
    exit 1
fi
echo "  baseline passes"
echo
echo "Each mutation reintroduces a real bug; the suite must catch it."

# R1: drop the h_finish gate, so a stale h_done hijacks any busy verb.
mutate "R1 stale h_done hijacks a verb" rtl/a2nix_card.v \
       's/end else if (h_finish \&\& h_done) begin/end else if (h_done) begin/'

# R2: remove the LEN index rewind, leaving no way to resynchronise it.
mutate "R2 no port-index rewind" rtl/a2nix_card.v \
       's/^                            lenidx <= 2.d0;/                            lenidx <= lenidx;/'

# R3: index P slots from the LSB, byte-reversing digests and path strings.
mutate "R3 P-slot byte order reversed" rtl/a2nix_card.v \
       's/R_PPORT: rdata = pslot\[psel\]\[255 - 8\*pidx -: 8\];/R_PPORT: rdata = pslot[psel][8*pidx +: 8];/'

# R4: lose pushed bytes by never advancing the FIFO write pointer.
#
# Finding a mutation for R4 took three attempts, and the failures are the useful part:
#
#   1. "READY reports only the hasher's state" SURVIVED. That was the original bug,
#      but the FIFO decoupled the CPU from the hasher and made it impossible.
#   2. "the drain issues a byte while one is in flight" ALSO SURVIVED. That hazard is
#      real in the RTL, but unreachable at this clock ratio: the bus delivers a byte
#      every ~48 card cycles while the drain consumes one every 2, so the FIFO is
#      essentially never non-empty across a block boundary.
#
# Conclusion worth recording: the 32-entry FIFO is heavily over-provisioned for a
# 1.024 MHz slot, so the whole overflow/double-issue hazard class is currently
# unreachable. Those guards stay as defence in depth -- they would matter on a slower
# card clock or a faster bus -- but they cannot be mutation-tested today, and
# pretending otherwise would be the decorative-test problem again.
#
# So R4 is mutated where byte delivery is genuinely load-bearing: the write pointer.
mutate "R4 pushed bytes overwrite each other" rtl/a2nix_card.v \
       's/^                                wptr            <= wptr + 6.d1;$/                                wptr            <= wptr;/'

# R5: let a new message inherit the previous one's partially-filled block, which is
# what "init is a no-op mid-message" amounted to.
mutate "R5 init keeps the partial block" rtl/sha256.v \
       's/^                bidx      <= 7.d0;$/                bidx      <= bidx;/'

# R5: let a new message inherit the previous length, so padding encodes the wrong
# bit count -- a valid hash of the wrong thing.
mutate "R5 init keeps the previous length" rtl/sha256.v \
       's/^                total     <= 64.d0;$/                total     <= total;/'

# R6: let a cyclic FOLLOW chain spin instead of reporting.
mutate "R6 cyclic FOLLOW never errors" rtl/a2nix_card.v \
       's/end else if (hops >= MAX_FOLLOW\[7:0\]) begin/end else if (1'"'"'b0) begin/'

# R6: swallow unknown verbs instead of reporting BADVERB.
mutate "R6 unknown verb silently ignored" rtl/a2nix_card.v \
       's/                                    errcode <= E_BADVERB;/                                    errcode <= E_NONE;/'

# L3: make the CAM's priority encoder return the HIGHEST matching lane. Attributes
# are sorted and callers disambiguate collisions by name, so an arbitrary match hands
# back a valid value from the wrong key.
mutate "L3 CAM returns the highest match" rtl/keycam.v \
       's/        for (j = WIDTH - 1; j >= 0; j = j - 1)/        for (j = 0; j < WIDTH; j = j + 1)/'

# L3: stop LOOKUP after the first CAM batch, so any attribute past lane 31 silently
# reports NOTFOUND.
mutate "L3 LOOKUP examines one batch only" rtl/a2nix_card.v \
       's/                        lk_done <= lk_done + {18.d0, lk_lane};/                        lk_done <= lk_count;/'

# L3: read the value column from the key column's base, returning a hash where a
# pointer belongs -- a plausible-looking address.
mutate "L3 LOOKUP reads the wrong column" rtl/a2nix_card.v \
       's/                        mcur     <= lk_values_at + ({8.d0, lk_hit_index} << 2);/                        mcur     <= lk_keys_at + ({8'"'"'d0, lk_hit_index} << 2);/'

echo
echo "$pass mutations caught, $fail not caught"
[ "$fail" -eq 0 ] || exit 1
echo "MUTATION PASS: every reintroduced bug is caught by the regression suite"
