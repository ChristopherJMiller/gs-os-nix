# gs-os-nix task runner
#
# Everything here assumes you are inside `nix develop` (or using direnv). The
# recipes are deliberately thin wrappers over the real tools -- if a recipe and
# `nix flake check` ever disagree, the flake is authoritative.
#
#   just            list tasks
#   just test       build and run the L1 hashing testbench (milestone M1)
#   just check      full flake check: parity, lint, synth
#   just resume     print where the project stands

RTL := "rtl/sha256_core.v rtl/sha256.v rtl/storepath.v rtl/keycam.v rtl/alu64.v rtl/a2nix_card.v"
TOPS := "sha256_core sha256 storepath keycam alu64 a2nix_card"
VECTORS := "tests/vectors/l1.txt"
SIM_DIR := "sim/build"
CARD_DIR := "sim/build_card"

# Show available tasks.
default:
    @just --list --unsorted

# --- Orientation -------------------------------------------------------------

# Print current layer status and the next thing to do.
resume:
    @echo "=== gs-os-nix ==="
    @sed -n '/^## Layer progress/,/^## Milestones/p' docs/STATUS.md
    @echo "Docs: ARCHITECTURE (shape) -> DECISIONS (why) -> CARD-ISA (contract)"

# Show the card's register map and verb table.
isa:
    @sed -n '/^## Register map/,/^## Unproven/p' docs/CARD-ISA.md

# --- L1: hashing RTL ---------------------------------------------------------

# Lint all RTL. Must stay clean: the RTL feeds both MAME and the bitstream.
# Each module is linted as its own top -- stricter, and avoids MULTITOP.
lint:
    #!/usr/bin/env bash
    set -euo pipefail
    for top in {{TOPS}}; do
        echo "=== lint $top ==="
        verilator --lint-only -Wall -Wno-DECLFILENAME --top-module "$top" {{RTL}}
    done
    echo "lint clean"

# Style-check RTL with verible (advisory, not gating).
style:
    -verible-verilog-lint {{RTL}}

# Build the Verilator testbench.
build:
    verilator --cc --exe --build -j 0 \
        -Irtl --top-module tb_top \
        -Wall -Wno-DECLFILENAME \
        --Mdir {{SIM_DIR}} -o l1_test \
        sim/tb_top.v sim/main.cpp

# Build and run the L1 testbench: NIST vectors + store-path parity (M1).
test: build
    ./{{SIM_DIR}}/l1_test {{VECTORS}}

# --- L2: card register interface ---------------------------------------------

# Build the card testbench, which drives $C0nX exactly as the MAME device will.
build-card:
    verilator --cc --exe --build -j 0 \
        -Irtl --top-module a2nix_card \
        -Wall -Wno-DECLFILENAME \
        --Mdir {{CARD_DIR}} -o card_test \
        rtl/a2nix_card.v sim/card_main.cpp

# Run the card testbench: register map, verb dispatch, and store-path parity
# through the documented CPU sequence. Also reports bus accesses per byte.
test-card: build-card
    ./{{CARD_DIR}}/card_test {{VECTORS}}

# Print just the verb-economics numbers (the D5 gate).
economics: build-card
    @./{{CARD_DIR}}/card_test {{VECTORS}} | grep -E "bus accesses|cost:"

# --- Regressions -------------------------------------------------------------

# Run the silent-corruption regression suite. Each test names the bug it guards.
regress:
    verilator --cc --exe --build -j 0 \
        -Irtl --top-module a2nix_card \
        -Wall -Wno-DECLFILENAME \
        --Mdir sim/build_regress -o regress \
        rtl/a2nix_card.v sim/regress_main.cpp
    ./sim/build_regress/regress {{VECTORS}}

# Prove the regression suite can fail: reintroduce each fixed bug, expect a catch.
# Run this whenever you add a regression test -- a test that cannot fail is worse
# than no test.
mutation:
    ./scripts/mutation-test.sh

# Everything: L1, L2, regressions, M2, the 65816 model.
test-all: test test-card regress m2 selftest bench-mul64-native

# Regenerate golden vectors from the REAL nix on this host.
# Aborts if the reference model disagrees with nix, so parity can't silently rot.
vectors:
    python3 scripts/gen-vectors.py {{VECTORS}}

# Show what the committed vectors currently assert.
show-vectors:
    @awk '/^SP/ {print "  " $4}' {{VECTORS}}

# --- 65816 target ------------------------------------------------------------

# Assemble the store-path demo to a raw binary loaded at $2000.
asm:
    mkdir -p target/build
    ca65 --cpu 65816 -o target/build/storepath_demo.o \
        -l target/build/storepath_demo.lst target/tests/storepath_demo.s
    ld65 -C target/tests/apple2.cfg -o target/build/storepath_demo.bin \
        -m target/build/storepath_demo.map target/build/storepath_demo.o
    @ls -l target/build/storepath_demo.bin

# Assemble the printing-free variant used by the automated M2 check. It writes
# its result to $0300 so the debugger can read it without scraping the screen.
asm-headless:
    mkdir -p target/build
    ca65 --cpu 65816 -D NO_PRINT -o target/build/storepath_demo_headless.o \
        target/tests/storepath_demo.s
    ld65 -C target/tests/apple2.cfg -o target/build/storepath_demo_headless.bin \
        -m target/build/storepath_demo_headless.map \
        target/build/storepath_demo_headless.o
    @ls -l target/build/storepath_demo_headless.bin

# Both variants.
asm-all: asm asm-headless

# Build a ProDOS image carrying the demo, for interactive runs.
disk: asm
    ./scripts/mkdisk.sh

# --- M2 ----------------------------------------------------------------------

# Every ROM-free run below goes through this binary.
# Build the minimal 65816 harness (sim/a2min.cpp) against the Verilated card.
build-a2min:
    verilator --cc --exe --build -j 0 \
        -Irtl --top-module a2nix_card \
        -Wall -Wno-DECLFILENAME \
        --Mdir sim/build_a2min -o a2min \
        rtl/a2nix_card.v sim/a2min.cpp

# Assemble target/tests/<name>.s to a raw $2000 binary in target/build/.
asm-test name:
    mkdir -p target/build
    ca65 --cpu 65816 -o target/build/{{name}}.o -l target/build/{{name}}.lst \
        target/tests/{{name}}.s
    ld65 -C target/tests/apple2.cfg -o target/build/{{name}}.bin \
        target/build/{{name}}.o

# Milestone M2, the ROM-free way: run the real binary on the minimal 65816 machine
# wired to the real RTL, and compare against the golden vector. No ROMs needed,
# so this is the gating check.
m2: asm-headless build-a2min
    ./sim/build_a2min/a2min target/build/storepath_demo_headless.bin {{VECTORS}} 4

# The expected values live in the harness, not in the assembly (see native_test.s).
# Self-test a2min's 65816 native-mode model: every addressing mode and mode switch.
selftest: build-a2min (asm-test "native_test")
    ./sim/build_a2min/a2min --selftest target/build/native_test.bin

# Prove the self-test can fail: break the interpreter 18 ways, expect 18 catches.
mutation-a2min: (asm-test "native_test")
    ./scripts/mutation-a2min.sh

# How many cycles does the 65816 spend on a 64x64 multiply by itself?
# The D5 gate for ALU64, 8-bit emulation mode (target/tests/mul64_bench.s).
bench-mul64 a="123456789" b="987654321": build-a2min (asm-test "mul64_bench")
    ./sim/build_a2min/a2min --bench-mul64 target/build/mul64_bench.bin {{a}} {{b}}

# This is the number the gate is judged against, since the VM runs in native mode.
# The D5 gate for ALU64, 16-bit native mode (target/tests/mul64_native.s).
bench-mul64-native a="123456789" b="987654321": build-a2min (asm-test "mul64_native")
    ./sim/build_a2min/a2min --bench-mul64-native target/build/mul64_native.bin {{a}} {{b}}

# Runs the nursery (target/heap/nursery.s) on a2min.
# The D6 measurement: what fraction of allocated bytes crossed the bus?
bench-nursery: build-a2min (asm-test "nursery_bench")
    ./sim/build_a2min/a2min --bench-nursery target/build/nursery_bench.bin

# M2 again, but on real MAME for higher fidelity (real slot timing, real IIgs).
# Needs a complete apple2gs romset: ROMPATH=~/mame/roms just m2-mame
m2-mame: asm-headless
    ./scripts/run-m2.sh

# Verify the demo's hardcoded fingerprint still matches the golden vectors.
check-demo-vector:
    python3 scripts/check-demo-vector.py

# --- MAME --------------------------------------------------------------------

# Build MAME with the a2nix card. Long one-off; cached afterwards.
mame-build:
    nix build .#mame-a2nix -L

# Confirm the card registered as an Apple IIgs slot option.
mame-slots:
    @nix build .#mame-a2nix 2>/dev/null || just mame-build
    ./result/bin/mame -listslots apple2gs 2>/dev/null | grep -i -A1 a2nix \
      || echo "a2nix not listed -- check the injection in nix/pkgs/mame-a2nix.nix"

# --- FPGA --------------------------------------------------------------------

# Synthesis smoke test for ECP5 (see DECISIONS.md D10 for why not ice40).
synth top="sha256":
    yosys -p "read_verilog {{RTL}}; synth_ecp5 -top {{top}}; stat"

# Report LUT/FF usage per module. Keep the table in ARCHITECTURE.md in sync with this.
area *tops:
    ./scripts/area.sh {{tops}}

# --- Waveforms ---------------------------------------------------------------

# Build a tracing testbench and open the waveform.
waves:
    verilator --cc --exe --build -j 0 --trace \
        -Irtl --top-module tb_top \
        -Wall -Wno-DECLFILENAME \
        --Mdir {{SIM_DIR}} -o l1_test \
        sim/tb_top.v sim/main.cpp
    ./{{SIM_DIR}}/l1_test {{VECTORS}}
    surfer dump.vcd

# --- Whole-project -----------------------------------------------------------

# Everything the CI gate runs.
check:
    nix flake check -L

# Format nix files.
fmt:
    nix fmt

# Remove build artifacts.
clean:
    rm -rf {{SIM_DIR}} obj_dir build *.vcd *.fst

# --- Housekeeping ------------------------------------------------------------

# Stage and commit with a message.
commit msg:
    git add -A
    git commit -m "{{msg}}"

# Compact log.
log:
    @git --no-pager log --oneline --decorate -20
