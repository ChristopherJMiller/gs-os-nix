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

# For the same question asked of real programs, see `just verb-economics`.

# The card-side micro-benchmark: bus accesses per verb (the D5 gate).
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

# Prove the self-test can fail: break the interpreter 20 ways, expect 20 catches.
mutation-a2min: (asm-test "native_test")
    ./scripts/mutation-a2min.sh

# The quiet clock (docs/RUNNING.md): every fixture and the M2 run,
# fast vs A2MIN_EXACT_CLOCK=1, must be byte-identical; then a broken copy must not be.
quiet-clock: build-a2min cvm nbc-build asm-headless
    python3 scripts/scoreboard.py --nbc {{NBC}} --a2min ./sim/build_a2min/a2min \
        --vm target/build/cvm.bin --corpus tests/fixtures --build target/build/nbc-fixtures \
        --expected tests/corpus/expected-pass.txt > /dev/null
    ./scripts/quiet-clock.sh ./sim/build_a2min/a2min target/build/cvm.bin \
        target/build/nbc-fixtures target/build/storepath_demo_headless.bin {{VECTORS}}

# float.c, built by ORCA/C, against the host's own IEEE-754 and printf, bit for
# bit (docs/FLOAT.md): three seeds of 1,120 vectors and a planted rounding bug.
float-vectors *seeds: build-a2min
    ./scripts/float-vectors.sh ./sim/build_a2min/a2min {{seeds}}

# The VM's C on the host, in seconds: clang -Wall -Wextra -Werror and gcsafe
# (no obj_t * local or view read across a collecting call), each self-tested.
c-lint:
    ./scripts/c-lint.sh

# Where the VM's code lands: bank 0's size, and every symbol in it with its
# size, must equal target/vm/bank0.txt exactly (after accounting for a change,
# `just bank-gate --update`); only the allowlisted runtime routines may be in
# it; it must end under the C stack's floor, which the stub and a2min's
# tripwire must agree on with vm.h (D31); and no symbol in a data bank.
bank-gate *flags: cvm
    python3 scripts/bank-gate.py target/build/cvm.bin target/build/cvm.map target/vm/bank0.txt {{flags}}

# ORCA/C's lint and the memory map's static asserts can fail: a planted
# violation of each, in a copy, must stop the build (D14).
orca-lint:
    ./scripts/orca-lint-selftest.sh

# Prove the collector fixtures can fail: break the remembered set six ways, object
# sizes three, the nursery reserves four, path + once, string coercion twice and
# SHA-256 three times.
# Mutates a copy, never the tree.
mutation-vm: build-a2min nbc-build
    ./scripts/mutation-vm.sh ./sim/build_a2min/a2min {{NBC}}

# The interpreter's speed against tests/bench/cycles.txt (scripts/bench.py, D29):
# slower fails; faster asks for `just bench-update`.
bench *flags: build-a2min cvm nbc-build
    python3 scripts/bench.py --nbc {{NBC}} --a2min ./sim/build_a2min/a2min --vm target/build/cvm.bin {{flags}}

bench-update: (bench "--update")

# The on-target compiler against nbc, stage by stage (docs/COMPILER.md, D33):
# every .nix in the repository, `fuzz` mutants of them, then planted bugs.
front-diff fuzz="20000": build-a2min cvm nbc-build
    python3 scripts/front-diff.py --nbc {{NBC}} --a2min ./sim/build_a2min/a2min --vm target/build/cvm.bin --fuzz {{fuzz}}
    python3 scripts/front-diff.py --self-test --nbc {{NBC}} --a2min ./sim/build_a2min/a2min --fuzz 2000

# Every deep program at every stack depth, under a2min's tripwire (VM.md "The C stack").
stack-stress: build-a2min cvm nbc-build
    ./scripts/stack-stress.sh ./sim/build_a2min/a2min {{NBC}} target/build/cvm.bin

# The phase counters for one .nix, bounded by cycles rather than wall clock (D19).
phases file budget="400000000": build-a2min cvm nbc-build
    mkdir -p target/build
    {{NBC}} {{file}} -o target/build/$(basename {{file}} .nix).nbc
    A2MIN_BUDGET={{budget}} ./sim/build_a2min/a2min --run-nbc target/build/cvm.bin \
        target/build/$(basename {{file}} .nix).nbc | grep -a "cycles\|phases\|profile\|\\$\|result\|error"

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

# --- C toolchain (D18, docs/TOOLCHAIN.md) -------------------------------------
# The devShell provides orcacc / orcalink / omf2bin / orcabuild / orcarun from the
# flake (ORCA/C's release is fetched by `nix build .#orcac`). These recipes use the
# tree's own harness source so a change to sim/orcarun.cpp is what runs.

ORCARUN := "sim/build_orca/orcarun"

# Build the harness that runs ORCA/C on this machine.
build-orcarun:
    mkdir -p sim/build_orca
    $CXX -O2 -std=c++17 -Wall -Wextra -o {{ORCARUN}} sim/orcarun.cpp

# Compile, link, flatten and run target/tests/smoke.c on a2min.
orca-smoke: build-a2min
    mkdir -p target/build
    orcabuild -o target/build/smoke.bin -O target/tests/smoke.c
    ./sim/build_a2min/a2min --run-bin target/build/smoke.bin

# --- The VM (docs/VM.md): C, compiled by ORCA/C ---------------------------------
# main.c stays first: the load file's entry point is the first function of the
# first object.
CVM_SRCS := `grep -v '^#' target/vm/c/SOURCES | sed 's|^|target/vm/c/|' | tr '\n' ' '`

# Compile, link and flatten the VM to target/build/cvm.bin.
cvm:
    mkdir -p target/build
    orcabuild -o target/build/cvm.bin -O {{CVM_SRCS}}

# Compile one .nix and run it on the VM.
run-nbc file: build-a2min cvm nbc-build
    mkdir -p target/build
    {{NBC}} {{file}} -o target/build/$(basename {{file}} .nix).nbc
    ./sim/build_a2min/a2min --run-nbc target/build/cvm.bin target/build/$(basename {{file}} .nix).nbc

# THE HEADLINE METRIC: run the vendored tvix corpus and the fixtures on the VM
# and count. tests/corpus/expected-pass.txt is the ratchet.
scoreboard *flags: build-a2min cvm nbc-build
    python3 scripts/scoreboard.py --nbc {{NBC}} --a2min ./sim/build_a2min/a2min \
        --vm target/build/cvm.bin --corpus tests/corpus/tvix --corpus tests/fixtures \
        --build target/build/nbc --expected tests/corpus/expected-pass.txt {{flags}}
    ./scripts/fixture-purity.sh target/build/nbc

scoreboard-update: (scoreboard "--update")

# Just the fixtures, normally and with a collection at every boundary.
fixtures *flags: build-a2min cvm nbc-build
    python3 scripts/scoreboard.py --nbc {{NBC}} --a2min ./sim/build_a2min/a2min \
        --vm target/build/cvm.bin --corpus tests/fixtures \
        --build target/build/nbc-fixtures --expected tests/corpus/expected-pass.txt {{flags}}

# With a collection at every boundary the slowest fixture needs ~290M cycles,
# within the default 500M budget.
fixtures-gc: build-a2min cvm nbc-build
    python3 scripts/scoreboard.py --nbc {{NBC}} \
        --a2min ./sim/build_a2min/a2min --vm target/build/cvm.bin --corpus tests/fixtures \
        --build target/build/nbc-fixtures --expected tests/corpus/expected-pass.txt --gc-stress

# GC stress at every k-th boundary: unlike k = 1, it leaves some stores young,
# so it exercises the remembered set (D20).
fixtures-gc-period k="3": build-a2min cvm nbc-build
    A2MIN_GC_PERIOD={{k}} python3 scripts/scoreboard.py --nbc {{NBC}} \
        --a2min ./sim/build_a2min/a2min --vm target/build/cvm.bin --corpus tests/fixtures \
        --build target/build/nbc-fixtures --expected tests/corpus/expected-pass.txt --gc-stress

# The fixtures with every nursery_reserve() collecting, needed or not (vm.h).
fixtures-reserve: build-a2min cvm nbc-build
    A2MIN_RESERVE_STRESS=1 python3 scripts/scoreboard.py --nbc {{NBC}} \
        --a2min ./sim/build_a2min/a2min --vm target/build/cvm.bin --corpus tests/fixtures \
        --build target/build/nbc-fixtures --expected tests/corpus/expected-pass.txt

# The fixtures through the card's moving collector: a nursery collection at
# every boundary, and every k-th of them followed by a collection of mature
# space (HEAP.md "Collecting mature space"), needed or not.
fixtures-mature k="1": build-a2min cvm nbc-build
    A2MIN_MATURE_STRESS={{k}} python3 scripts/scoreboard.py --nbc {{NBC}} \
        --a2min ./sim/build_a2min/a2min --vm target/build/cvm.bin --corpus tests/fixtures \
        --build target/build/nbc-fixtures --expected tests/corpus/expected-pass.txt --gc-stress

# The fixtures with a one-entry remembered set, so it fills (D20). Not under GC
# stress, which promotes every value before it can be stored young.
fixtures-rset: build-a2min cvm nbc-build
    A2MIN_RSET_LIMIT=1 python3 scripts/scoreboard.py --nbc {{NBC}} \
        --a2min ./sim/build_a2min/a2min --vm target/build/cvm.bin --corpus tests/fixtures \
        --build target/build/nbc-fixtures --expected tests/corpus/expected-pass.txt

# Promotion at every boundary is the default, because without it nothing on a
# fixture is ever mature and LOOKUP is never called at all (scripts/verb-economics.py).

# Price each card verb against the CPU doing the same work, on real programs.
verb-economics *flags: build-a2min cvm nbc-build
    python3 scripts/verb-economics.py --a2min ./sim/build_a2min/a2min \
        --vm target/build/cvm.bin --nbc-dir target/build/nbc-fixtures \
        --only "{{VERB_WORKLOAD}}" --nbc {{NBC}} --crossover 8,32,64,128 {{flags}}

# Run ORCA/C on one file with the call trace on, using the tree's harness.
orca-trace file: build-orcarun
    #!/usr/bin/env bash
    set -euo pipefail
    orca=$(nix build --no-link --print-out-paths .#orcac)/orca
    work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
    base=$(basename {{file}}); base=${base%.*}
    tr '\n' '\r' < {{file}} > "$work/$base.c#B00008"
    {{ORCARUN}} --trace --orca "$orca" --map "0=$work" --sfile "$base.c" --dfile "$base" \
        --lops 1 --merr 8 --minus i "$orca/Languages/cc#B5DB01" compile "$base.c" "keep=$base"
    ls -la "$work"

# --- L4: bytecode VM ---------------------------------------------------------

NBC := "tools/nbc/target/release/nbc"

# The fixtures that actually reach a card verb: attribute lookups for LOOKUP,
# 64-bit multiply and divide for ALU64. Five configurations x every fixture
# under GC stress would be an hour for no extra information.
VERB_WORKLOAD := "attrs.nbc,attr-builtins.nbc,with.nbc,with-captured.nbc,hash-collision.nbc,functor.nbc,tryeval.nbc,carry-chain.nbc,div-truncates.nbc,int-limits.nbc,hashstring.nbc,derivation.nbc"

# Build the test-only .nbc fixture generator (Rust, no dependencies; D7/D16).
nbc-build:
    cd tools/nbc && cargo build --release --quiet

# Run the generator's unit tests: precedence, thunk placement, container round-trip.
nbc-test:
    cd tools/nbc && cargo test --quiet

# Compile one .nix file to target/build/<name>.nbc and disassemble it.
nbc file: nbc-build
    mkdir -p target/build
    {{NBC}} {{file}} -o target/build/$(basename {{file}} .nix).nbc
    {{NBC}} --dump target/build/$(basename {{file}} .nix).nbc

# D7: the generator has no hashing code and no fixture smuggles a store path.
purity:
    ./scripts/fixture-purity.sh --selftest
    ./scripts/fixture-purity.sh tests/fixtures target/build/nbc

# The opcode table must be one table across BYTECODE.md, opcodes.rs, opcodes.h.
opcodes:
    python3 scripts/check-opcodes.py

# M2 again, but on real MAME for higher fidelity (real slot timing, real IIgs).
# Needs your own complete apple2gs romset, given explicitly (no default):
#     ROMPATH=/path/to/your/roms just m2-mame
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
