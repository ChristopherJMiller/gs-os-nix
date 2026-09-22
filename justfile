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

RTL := "rtl/sha256_core.v rtl/sha256.v rtl/storepath.v rtl/a2nix_card.v"
TOPS := "sha256_core sha256 storepath a2nix_card"
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

# Everything: L1 and L2 testbenches.
test-all: test test-card

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
