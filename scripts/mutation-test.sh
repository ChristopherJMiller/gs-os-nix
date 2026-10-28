#!/usr/bin/env bash
# Prove the regression suite can actually FAIL.
#
# A regression test that cannot distinguish a working card from a broken one is worse
# than no test: it produces confidence without evidence. So this reintroduces each
# fixed bug into the RTL, one at a time, and asserts that sim/regress_main.cpp catches
# it. If a mutation survives, the corresponding test is decorative and must be fixed.
#
# Everything runs in a COPY of rtl/, sim/ and tests/ in a temporary directory, so
# the tree is never edited. This script used to mutate rtl/*.v in place and
# restore them from a trap -- but a SIGKILL (the low-memory reaper) skips every
# trap, which is how scripts/mutation-vm.sh twice left a mutation behind.
set -uo pipefail

WORK=$(mktemp -d)
cp -r rtl sim tests "$WORK/"
cd "$WORK" || exit 1

FILES=(rtl/a2nix_card.v rtl/sha256.v rtl/sha256_core.v rtl/storepath.v rtl/keycam.v rtl/alu64.v)
BACKUP_DIR=$(mktemp -d)
restore() { for f in "${FILES[@]}"; do cp "$BACKUP_DIR/$(basename "$f")" "$f"; done; }
trap 'rm -rf "$BACKUP_DIR" "$WORK"' EXIT
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
       's/mem_addr <= lk_values_at + ({8.d0, lk_hit_index} << 2);/mem_addr <= lk_keys_at + ({8'"'"'d0, lk_hit_index} << 2);/'

# L3: count the CPU's visit mark (bit 23 of aux, set while it deep-forces a set) as
# part of the attribute count, so a SELECT inside that deep force reads its value
# from 4 * 2^23 bytes past the set: null, or a real pointer from the wrong object.
mutate "L3 LOOKUP counts the visit mark" rtl/a2nix_card.v \
       's/                    lk_count <= {8.d0, mword\[23:8\]};/                    lk_count <= mword[31:8];/'

# ALU64: never add the partial product, so every multiply returns zero -- or, with a
# multiplicand of 1, the right answer, which is why the tests use awkward operands.
mutate "ALU64 MUL never accumulates" rtl/alu64.v \
       's/                        if (x\[0\]) acc <= acc + y;/                        if (1'"'"'b0) acc <= acc + y;/'

# ALU64: give the remainder the DIVISOR's sign instead of the dividend's. Every
# positive case still passes; only mixed-sign MOD reveals it.
mutate "ALU64 MOD takes the wrong sign" rtl/alu64.v \
       's/                            neg_r <= a\[63\];/                            neg_r <= b[63];/'

# ALU64: never negate the quotient, so -7/2 comes back as 3 -- right magnitude,
# wrong sign.
mutate "ALU64 DIV drops the quotient sign" rtl/alu64.v \
       's/                            neg_q <= a\[63\] ^ b\[63\];/                            neg_q <= 1'"'"'b0;/'

# ALU64: let x/0 return a value instead of an error.
mutate "ALU64 divide by zero not flagged" rtl/alu64.v \
       's/                            err    <= 1.b1;        \/\/ loud, never a wrap/                            err    <= 1'"'"'b0;/'

# ALU64: compare by subtraction, which overflows for operands of opposite sign.
mutate "ALU64 CMP overflows on opposite signs" rtl/alu64.v \
       's/    wire a_lt_b = (\$signed(a) < \$signed(b));/    wire [63:0] a_minus_b = a - b; wire a_lt_b = a_minus_b[63];/'

# ALU64: decode the op from the wrong nibble, so everything becomes ADD. This is the
# bug the first build actually had: six regression failures from one constant.
mutate "ALU64 op decoded from the wrong nibble" rtl/a2nix_card.v \
       's/        .op     (pslot\[2\]\[251:248\]),/        .op     (pslot[2][255:252]),/'

# GC: never leave a forwarding pointer -- the header write lands the last copied
# word instead of the FORWARDED tag -- so a shared object is copied once per
# reference and a cycle never terminates. (The replacement must still BUILD: a
# mutation that fails to compile is counted "caught" for the wrong reason.)
mutate "GC no forwarding pointer" rtl/a2nix_card.v \
       's/                        mem_wdata <= {24.d0, TAG_FORWARDED};/                        mem_wdata <= mem_wdata;/'

# GC: trace the ATTRS hash column as if it held pointers.
mutate "GC traces ATTRS hashes as pointers" rtl/a2nix_card.v \
       's/                TAG_ATTRS:              is_ptr_field = (k >= aux) \&\& (k < aux + (aux << 1));/                TAG_ATTRS:              is_ptr_field = (k < aux + (aux << 1));/'

# GC: skip the Cheney scan entirely, so nested pointers keep pointing at from-space.
mutate "GC traces a closure's word 0, the VM's code offsets" rtl/a2nix_card.v \
       's/                TAG_THUNK, TAG_LAMBDA, TAG_BLACKHOLE: is_ptr_field = (k == 32.d1);/                TAG_THUNK, TAG_LAMBDA, TAG_BLACKHOLE: is_ptr_field = (k < 32'"'"'d2);/'

mutate "GC counts the CPU's marks as length" rtl/a2nix_card.v \
       '0,/            aux = {16.d0, hdr\[23:8\]};/ s//            aux = {8'"'"'d0, hdr[31:8]} \& 32'"'"'h007F_FFFF;/'

mutate "GC copies a BLACKHOLE as 8 bytes" rtl/a2nix_card.v \
       's/                TAG_THUNK, TAG_LAMBDA, TAG_BLACKHOLE: s = 32.d12;/                TAG_THUNK, TAG_LAMBDA: s = 32'"'"'d12;/'

mutate "GC skips the scan" rtl/a2nix_card.v \
       's/                        gc_scan <= gc_scan + sc_size;/                        gc_scan <= gc_free;/'

# GC: never write the root's new address back. Range-addressed to the ROOT_WB state,
# since "mem_we <= 1'b1" appears in many states.
mutate "GC roots not written back" rtl/a2nix_card.v \
       '/M_GC_ROOT_WB: begin/,/mstate    <= M_GC_ROOT;/ s/                    mem_we    <= 1.b1;/                    mem_we    <= 1'"'"'b0;/'

# RESERVE: size the block like an ALLOC (header included), shifting every pointer
# the promoter translates by a constant offset.
mutate "RESERVE adds a header's worth of padding" rtl/a2nix_card.v \
       's/                                        alloc_ptr <= alloc_ptr + alloc_total(len) - 32.d4;/                                        alloc_ptr <= alloc_ptr + alloc_total(len);/'

# MEMCMP: compare the raw little-endian words instead of reversing them into
# address order. Names then sort by their LAST byte first, so every merge of two
# sorted attribute sets silently reorders -- and the sign is right often enough
# to look fine.
mutate "MEMCMP compares little-endian words, not address order" rtl/a2nix_card.v \
       's/{ mc_aw\[7:0\], mc_aw\[15:8\], mc_aw\[23:16\], mc_aw\[31:24\] }/mc_aw/; s/{ mword\[7:0\], mword\[15:8\], mword\[23:16\], mword\[31:24\] }/mword/'

# MEMCMP: stop masking the final partial word, so the comparison runs past LEN
# into whatever follows the string in the heap.
mutate "MEMCMP reads past LEN in the last word" rtl/a2nix_card.v \
       's/            32.d3:   mc_mask = 32.hFFFF_FF00;/            32'"'"'d3:   mc_mask = 32'"'"'hFFFF_FFFF;/'

# MEMCMP: answer after the first word, so any two names sharing a four-byte
# prefix confirm as equal -- plausibly the wrong attribute.
mutate "MEMCMP stops after the first word" rtl/a2nix_card.v \
       '/M_MC_CMP: begin/,/mstate <= M_MC_A;/ s/if (mc_rem <= 32.d4) begin/if (1) begin/'

# MEMCMP: accept an unaligned operand and compare the word that contains it,
# answering about bytes the caller never asked about.
mutate "MEMCMP accepts an unaligned operand" rtl/a2nix_card.v \
       's/if (areg\[0\]\[1:0\] != 2.b00 || areg\[1\]\[1:0\] != 2.b00) begin/if (1'"'"'b0) begin/'

echo
echo "$pass mutations caught, $fail not caught"
[ "$fail" -eq 0 ] || exit 1
echo "MUTATION PASS: every reintroduced bug is caught by the regression suite"
