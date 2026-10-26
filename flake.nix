{
  description = "Nix on an Apple IIgs, with a verb-based FPGA accelerator card";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];

      # Our own packages, built once per system and shared by devShells, packages
      # and checks so nothing drifts between them.
      ourPackagesFor = pkgs: nixpkgs.lib.makeScope pkgs.newScope (self': {
        applecommander = self'.callPackage ./nix/pkgs/applecommander.nix { };
        cadius = self'.callPackage ./nix/pkgs/cadius.nix { };
        verilated-card = self'.callPackage ./nix/pkgs/verilated-card.nix {
          rtlSrc = ./rtl;
        };
        mame-a2nix = self'.callPackage ./nix/pkgs/mame-a2nix.nix {
          verilatedCard = self'.verilated-card;
          cardSrc = ./mame;
        };
        # Host-side, test-only fixture generator (D7, D16).
        nbc = self'.callPackage ./nix/pkgs/nbc.nix { };
        # The C toolchain (D18, docs/TOOLCHAIN.md): ORCA/C's official release,
        # the harness that runs it here, and the commands that wrap both.
        orcac = self'.callPackage ./nix/pkgs/orcac.nix { };
        orcarun = self'.callPackage ./nix/pkgs/orcarun.nix { };
        orca-toolchain = self'.callPackage ./nix/pkgs/orca-toolchain.nix { };
        mkOrcaBinary = self'.callPackage ./nix/lib/mkOrcaBinary.nix { };
        # The VM itself (docs/VM.md): C, compiled by ORCA/C, flattened for a2min.
        # `nix build .#cvm` -> result/cvm.bin. The sources, in link order, are
        # target/vm/c/SOURCES -- the one list every build reads.
        cvm = self'.mkOrcaBinary {
          name = "cvm";
          srcs = map (f: ./target/vm/c + "/${f}")
            (builtins.filter (l: l != "" && builtins.substring 0 1 l != "#")
              (nixpkgs.lib.splitString "\n" (builtins.readFile ./target/vm/c/SOURCES)));
          includes = [ ./target/vm/c ];
        };
      });

      forAll = f: nixpkgs.lib.genAttrs systems (system:
        let pkgs = import nixpkgs {
          inherit system;
          # ORCA/C's licence permits use but not redistribution (nix/pkgs/orcac.nix);
          # it is the one unfree package here, named explicitly.
          config.allowUnfreePredicate = p: nixpkgs.lib.getName p == "orcac";
        };
        in f {
          inherit system pkgs;
          ours = ourPackagesFor pkgs;
        });

      # Every RTL module, in dependency order.
      rtlSources = "rtl/sha256_core.v rtl/sha256.v rtl/storepath.v rtl/keycam.v rtl/alu64.v"
        + " rtl/a2nix_card.v";
      rtlTops = [ "sha256_core" "sha256" "storepath" "keycam" "alu64" "a2nix_card" ];
    in
    {
      devShells = forAll ({ pkgs, ours, ... }: {
        default = pkgs.mkShell {
          name = "gs-os-nix";

          packages = with pkgs; [
            # --- RTL: simulate, lint, synthesise ---
            verilator            # also the MAME device's source of truth
            iverilog
            verible              # style/lint
            yosys                # synth smoke test
            nextpnr              # place & route
            trellis              # ECP5 bitstream (see DECISIONS.md D10)
            openfpgaloader       # programming, when real hardware exists
            surfer               # waveform viewer
            gtkwave

            # --- Python testbenches and vector generation ---
            # Pinned to 3.13: cocotb 2.x does not support the 3.14 default interpreter.
            # libclang: scripts/hostlint/gcsafe.py (just c-lint).
            (python313.withPackages (ps: with ps; [ cocotb pytest libclang ]))

            # --- 65816 target ---
            cc65                 # ca65 --cpu 65816; the C compiler is 6502-only
            llvmPackages.clang-unwrapped   # host lint of the VM's C (just c-lint); unwrapped,
                                           # so it adds no `cc` to shadow gcc's

            # --- Host tools that never touch the Verilated model (D16) ---
            cargo                # tools/nbc, the test-only fixture generator
            rustc
            rustfmt
            clippy

            # --- Emulation ---
            mame                 # stock; `nix build .#mame-a2nix` adds the card

            # --- Apple II disk images (packaged here; absent from nixpkgs) ---
            ours.applecommander  # `ac`
            ours.cadius
            ours.orca-toolchain  # orcacc / orcalink / omf2bin / orcabuild / orcarun (D18)

            # --- Build glue ---
            just                 # see ./justfile for the task list
            gnumake
            pkg-config
          ];

          shellHook = ''
            echo "gs-os-nix  --  see docs/STATUS.md to resume"
            echo "  ca65      $(ca65 --version 2>&1 | head -1)"
            echo "  verilator $(verilator --version 2>&1 | head -1)"
            echo "  yosys     $(yosys -V 2>&1 | head -1)"
          '';
        };
      });

      checks = forAll ({ pkgs, ours, ... }: {

        # --- L4: bytecode VM ---------------------------------------------------

        # The fixture generator's unit tests: lexer, Nix precedence, thunk
        # placement, the .nbc container round-trip. Building the package runs
        # `cargo test` (nix/pkgs/nbc.nix).
        nbc-tests = ours.nbc;

        # The opcode table lives in BYTECODE.md, opcodes.rs and opcodes.h. A
        # disagreement between generator and VM would yield fixtures that run and
        # return plausible wrong answers, so the three are diffed mechanically.
        nbc-opcodes = pkgs.runCommand "check-nbc-opcodes"
          { nativeBuildInputs = [ pkgs.python3 ]; }
          ''
            cp -r ${self}/docs ${self}/tools ${self}/target ${self}/scripts ${self}/sim .
            chmod -R +w .
            python3 scripts/check-opcodes.py --selftest | tee "$out"
            python3 scripts/check-opcodes.py | tee -a "$out"
            grep -q "^NBC OPCODES SELFTEST OK" "$out"
            grep -q "^NBC OPCODES OK" "$out"
          '';

        # D7 made mechanical: the generator contains no hashing or store-path code,
        # and no fixture carries a store path or digest its source does not. The
        # checker self-tests first (plants each violation, expects a catch), then
        # runs for real.
        fixture-purity = pkgs.runCommand "check-fixture-purity" { }
          ''
            cp -r ${self}/tools ${self}/tests ${self}/scripts .
            chmod -R +w .
            patchShebangs scripts
            ./scripts/fixture-purity.sh --selftest | tee "$out"
            ./scripts/fixture-purity.sh tests/fixtures | tee -a "$out"
          '';

        # THE HEADLINE METRIC. Compile every vendored tvix eval-okay test with the
        # generator, run each on the 65816 VM under a2min, compare with the .exp
        # file. tests/corpus/expected-pass.txt lists what passes today; a test
        # dropping out of it fails the check, so the number can only rise. The
        # compiled fixtures are then purity-checked against their sources.
        scoreboard = pkgs.runCommandCC "check-scoreboard"
          { nativeBuildInputs = [ pkgs.verilator pkgs.python3 ours.nbc ]; }
          ''
            cp -r ${self}/rtl ${self}/sim ${self}/tests ${self}/scripts .
            chmod -R +w .
            patchShebangs scripts
            verilator --cc --exe --build -j $NIX_BUILD_CORES \
              -Irtl --top-module a2nix_card \
              -Wall -Wno-DECLFILENAME \
              --Mdir build -o a2min \
              rtl/a2nix_card.v sim/a2min.cpp
            vm=${ours.cvm}/cvm.bin
            python3 scripts/scoreboard.py \
              --nbc nbc --a2min ./build/a2min --vm "$vm" \
              --corpus tests/corpus/tvix --corpus tests/fixtures \
              --build build/nbc \
              --expected tests/corpus/expected-pass.txt | tee "$out"
            grep -q "^SCOREBOARD:" "$out"
            ./scripts/fixture-purity.sh build/nbc | tee -a "$out"

            # The collector test: every local fixture again, with a nursery
            # collection at every bytecode boundary. A fixture that passes
            # normally and fails here has a root the collector cannot see. The
            # default 500M-cycle budget covers the slowest (big-operations, ~290M).
            python3 scripts/scoreboard.py \
              --nbc nbc --a2min ./build/a2min --vm "$vm" \
              --corpus tests/fixtures --build build/gc \
              --expected tests/corpus/expected-pass.txt --gc-stress \
              | tee -a "$out" | grep -q "^SCOREBOARD: [0-9]* / [0-9]* pass (0 fail, 0 error"

            # And collecting at every 3rd boundary instead (A2MIN_GC_PERIOD): a
            # collection at EVERY boundary promotes each value before it can be
            # stored, so it never exercises the remembered set (D20); a period
            # leaves some stores young, including into a promoted `let`.
            A2MIN_GC_PERIOD=3 python3 scripts/scoreboard.py \
              --nbc nbc --a2min ./build/a2min --vm "$vm" \
              --corpus tests/fixtures --build build/gc3 \
              --expected tests/corpus/expected-pass.txt --gc-stress \
              | tee -a "$out" | grep -q "^SCOREBOARD: [0-9]* / [0-9]* pass (0 fail, 0 error"

            # And with every nursery_reserve() collecting (A2MIN_RESERVE_STRESS):
            # each reserve is a mid-instruction collection at a point its caller
            # claims holds nothing live in a C local, and this is what tests it.
            A2MIN_RESERVE_STRESS=1 python3 scripts/scoreboard.py \
              --nbc nbc --a2min ./build/a2min --vm "$vm" \
              --corpus tests/fixtures --build build/reserve \
              --expected tests/corpus/expected-pass.txt \
              | tee -a "$out" | grep -q "^SCOREBOARD: [0-9]* / [0-9]* pass (0 fail, 0 error"

            # Once more with the remembered set shrunk to one entry (D20), so a
            # second young store finds it full and collects mid-instruction -- a
            # path the fixtures never take otherwise. A normal run, not GC
            # stress: promoting at every boundary makes every value mature
            # before it is stored, so under stress the set is never used.
            A2MIN_RSET_LIMIT=1 python3 scripts/scoreboard.py \
              --nbc nbc --a2min ./build/a2min --vm "$vm" \
              --corpus tests/fixtures --build build/rset \
              --expected tests/corpus/expected-pass.txt \
              | tee -a "$out" | grep -q "^SCOREBOARD: [0-9]* / [0-9]* pass (0 fail, 0 error"

            # The ratchet must be able to fail: a listed test that no longer
            # passes is a regression, and the run must say so and exit non-zero.
            # Plant one by giving a real fixture the wrong expected value.
            mkdir -p ghost && cp tests/fixtures/eval-okay-let-forward.* ghost/
            echo "wrong" > ghost/eval-okay-let-forward.exp
            echo "ghost/eval-okay-let-forward" > ghost.txt
            if python3 scripts/scoreboard.py \
                 --nbc nbc --a2min ./build/a2min --vm "$vm" \
                 --corpus ghost --build build/ghost --expected ghost.txt \
                 > ghost.log 2>&1; then
              echo "scoreboard ratchet SURVIVED a planted regression" >&2; exit 1
            fi
            grep -q "^REGRESSION:" ghost.log
            echo "ratchet self-test: a planted regression is caught" | tee -a "$out"
          '';

        # The verb gate (DECISIONS.md, CARD-ISA.md): a verb must be justified by a
        # measured comparison against the 65816 doing the same work, or it gets
        # deleted. `just economics` does that card-side on synthetic operands;
        # this does it on real programs, by running the same fixtures with each
        # verb forced onto its CPU path (vm.h VERB_NO_*).
        #
        # It is also a correctness check, and that is the part that can fail:
        # moving one operation to the CPU must not change any answer. If the
        # card's LOOKUP and the CPU's scan, or ALU64 and ORCA/C's own long-long
        # helpers, ever disagree, one of them is wrong -- and a wrong answer
        # matters far more than the cycle counts this check prints.
        verb-economics = pkgs.runCommandCC "check-verb-economics"
          { nativeBuildInputs = [ pkgs.verilator pkgs.python3 ours.nbc ]; }
          ''
            cp -r ${self}/rtl ${self}/sim ${self}/tests ${self}/scripts .
            chmod -R +w .
            patchShebangs scripts
            verilator --cc --exe --build -j $NIX_BUILD_CORES \
              -Irtl --top-module a2nix_card \
              -Wall -Wno-DECLFILENAME \
              --Mdir build -o a2min \
              rtl/a2nix_card.v sim/a2min.cpp
            # The fixtures that actually reach a verb: attribute lookups for
            # LOOKUP, 64-bit multiply and divide for ALU64, SHA-256 and store
            # paths for HASH. Six configurations times every fixture under GC
            # stress would be an hour for no more information (justfile
            # VERB_WORKLOAD is the same list).
            mkdir -p build/nbc
            for f in attrs attr-builtins with with-captured hash-collision \
                     functor tryeval carry-chain div-truncates int-limits hashstring derivation; do
              nbc "tests/fixtures/eval-okay-$f.nix" -o "build/nbc/$f.nbc"
            done
            python3 scripts/verb-economics.py \
              --a2min ./build/a2min --vm ${ours.cvm}/cvm.bin \
              --nbc-dir build/nbc --nbc nbc --crossover 8,32,64,128 | tee "$out"
            grep -q "^VERB ECONOMICS OK" "$out"
          '';

        # M1: the RTL's store paths must be byte-identical to upstream nix.
        #
        # The vectors are a CHECKED-IN golden artifact rather than generated here,
        # because a derivation cannot invoke `nix eval` to ask the real binary for
        # ground truth. Parity is established when scripts/gen-vectors.py runs (it
        # aborts if its reference model disagrees with nix) and is re-verifiable any
        # time with `nix run .#gen-vectors`.
        l1-hashing = pkgs.runCommandCC "check-l1-hashing"
          {
            # python3 is required by verilator's own verilator_includer at build time,
            # not by anything we wrote.
            nativeBuildInputs = [ pkgs.verilator pkgs.python3 ];
          }
          ''
            cp -r ${self}/rtl ${self}/sim ${self}/tests .
            chmod -R +w .
            verilator --cc --exe --build -j $NIX_BUILD_CORES \
              -Irtl --top-module tb_top \
              -Wall -Wno-DECLFILENAME \
              --Mdir build -o l1_test \
              sim/tb_top.v sim/main.cpp
            ./build/l1_test tests/vectors/l1.txt | tee "$out"
            grep -q "M1 PASS" "$out"
          '';

        # L2: the card's $C0nX register map and verb dispatch, driven through exactly
        # the access pattern MAME uses and the CPU sequence in CARD-ISA.md. Proves the
        # card before any MAME glue exists, so an M2 failure localises to the glue.
        l2-card = pkgs.runCommandCC "check-l2-card"
          { nativeBuildInputs = [ pkgs.verilator pkgs.python3 ]; }
          ''
            cp -r ${self}/rtl ${self}/sim ${self}/tests .
            chmod -R +w .
            verilator --cc --exe --build -j $NIX_BUILD_CORES \
              -Irtl --top-module a2nix_card \
              -Wall -Wno-DECLFILENAME \
              --Mdir build -o card_test \
              rtl/a2nix_card.v sim/card_main.cpp
            ./build/card_test tests/vectors/l1.txt | tee "$out"
            grep -q "L2 CARD PASS" "$out"
          '';

        # M2: real 65816 machine code, executing real bus cycles, drives the real RTL
        # to a store path identical to upstream nix.
        #
        # Runs on sim/a2min.cpp rather than MAME because apple2gs cannot start without
        # all four of its ROMs -- one of which is only the character-generator font
        # (region "gfx1"), irrelevant to a headless run but still mandatory, and not
        # redistributable. This keeps M2 verifiable on any machine.
        m2-storepath = pkgs.runCommandCC "check-m2-storepath"
          { nativeBuildInputs = [ pkgs.verilator pkgs.python3 pkgs.cc65 ]; }
          ''
            cp -r ${self}/rtl ${self}/sim ${self}/tests ${self}/target .
            chmod -R +w .
            mkdir -p target/build
            ca65 --cpu 65816 -D NO_PRINT \
              -o target/build/demo.o target/tests/storepath_demo.s
            ld65 -C target/tests/apple2.cfg \
              -o target/build/storepath_demo_headless.bin target/build/demo.o
            verilator --cc --exe --build -j $NIX_BUILD_CORES \
              -Irtl --top-module a2nix_card \
              -Wall -Wno-DECLFILENAME \
              --Mdir build -o a2min \
              rtl/a2nix_card.v sim/a2min.cpp
            ./build/a2min target/build/storepath_demo_headless.bin \
              tests/vectors/l1.txt 4 | tee "$out"
            grep -q "M2 PASS" "$out"
          '';

        # a2min clocks the card only while that can change something (the quiet
        # clock, docs/RUNNING.md). Prove it changes no output: every fixture,
        # the card verbs on, and the M2 run, against A2MIN_EXACT_CLOCK=1, byte for
        # byte -- then prove the comparison catches a broken quiet clock.
        quiet-clock = pkgs.runCommandCC "check-quiet-clock"
          { nativeBuildInputs = [ pkgs.verilator pkgs.python3 pkgs.cc65 ours.nbc ]; }
          ''
            cp -r ${self}/rtl ${self}/sim ${self}/tests ${self}/target ${self}/scripts .
            chmod -R +w .
            patchShebangs scripts
            mkdir -p target/build
            ca65 --cpu 65816 -D NO_PRINT \
              -o target/build/demo.o target/tests/storepath_demo.s
            ld65 -C target/tests/apple2.cfg \
              -o target/build/storepath_demo_headless.bin target/build/demo.o
            verilator --cc --exe --build -j $NIX_BUILD_CORES \
              -Irtl --top-module a2nix_card \
              -Wall -Wno-DECLFILENAME \
              --Mdir build -o a2min \
              rtl/a2nix_card.v sim/a2min.cpp
            python3 scripts/scoreboard.py --nbc nbc --a2min ./build/a2min \
              --vm ${ours.cvm}/cvm.bin --corpus tests/fixtures --build build/nbc \
              --expected tests/corpus/expected-pass.txt > /dev/null
            ./scripts/quiet-clock.sh ./build/a2min ${ours.cvm}/cvm.bin build/nbc \
              target/build/storepath_demo_headless.bin tests/vectors/l1.txt | tee "$out"
            grep -q "QUIET CLOCK OK" "$out"
          '';

        # D6: the generational boundary is the slot connector. Runs the CPU-side
        # nursery (target/heap/nursery.s) on the 65816 harness, verifies the promoted
        # graph arrived on the card intact with every young pointer translated, and
        # asserts the bus carried each surviving byte once plus a small fixed cost.
        d6-nursery = pkgs.runCommandCC "check-d6-nursery"
          { nativeBuildInputs = [ pkgs.verilator pkgs.python3 pkgs.cc65 ]; }
          ''
            cp -r ${self}/rtl ${self}/sim ${self}/tests ${self}/target .
            chmod -R +w .
            mkdir -p target/build
            ca65 --cpu 65816 -o target/build/nursery_bench.o target/tests/nursery_bench.s
            ld65 -C target/tests/apple2.cfg \
              -o target/build/nursery_bench.bin target/build/nursery_bench.o
            verilator --cc --exe --build -j $NIX_BUILD_CORES \
              -Irtl --top-module a2nix_card \
              -Wall -Wno-DECLFILENAME \
              --Mdir build -o a2min \
              rtl/a2nix_card.v sim/a2min.cpp
            ./build/a2min --bench-nursery target/build/nursery_bench.bin | tee "$out"
            grep -q "BENCH OK" "$out"
          '';

        # The 65816 model the L4 VM runs on. native_test.s exercises every native-mode
        # addressing mode and mode switch; the expected values live in the harness,
        # worked out from the data sheet, so the test and the interpreter cannot share
        # a mistake. Then both mul64 benchmarks, whose cycle counts are the D5 gate for
        # ALU64 (CARD-ISA.md quotes them; keep it in sync).
        a2min-native = pkgs.runCommandCC "check-a2min-native"
          { nativeBuildInputs = [ pkgs.verilator pkgs.python3 pkgs.cc65 ]; }
          ''
            cp -r ${self}/rtl ${self}/sim ${self}/tests ${self}/target .
            chmod -R +w .
            mkdir -p target/build
            for t in native_test mul64_native mul64_bench; do
              ca65 --cpu 65816 -o target/build/$t.o target/tests/$t.s
              ld65 -C target/tests/apple2.cfg -o target/build/$t.bin target/build/$t.o
            done
            verilator --cc --exe --build -j $NIX_BUILD_CORES \
              -Irtl --top-module a2nix_card \
              -Wall -Wno-DECLFILENAME \
              --Mdir build -o a2min \
              rtl/a2nix_card.v sim/a2min.cpp
            ./build/a2min --selftest target/build/native_test.bin \
              | tee "$out" | grep -q "SELFTEST OK"
            ./build/a2min --bench-mul64-native target/build/mul64_native.bin \
              123456789 987654321 | tee -a "$out" | grep -q "BENCH OK"
            ./build/a2min --bench-mul64 target/build/mul64_bench.bin \
              123456789 987654321 | tee -a "$out" | grep -q "BENCH OK"
          '';

        # And prove that self-test can fail: break the interpreter one feature at a
        # time and expect every break to be caught. Same rationale as `mutation`.
        mutation-a2min = pkgs.runCommandCC "check-mutation-a2min"
          { nativeBuildInputs = [ pkgs.verilator pkgs.python3 pkgs.cc65 ]; }
          ''
            cp -r ${self}/rtl ${self}/sim ${self}/tests ${self}/target ${self}/scripts .
            chmod -R +w .
            patchShebangs scripts
            ./scripts/mutation-a2min.sh | tee "$out"
            grep -q "MUTATION PASS" "$out"
          '';

        # And the VM's collector (DECISIONS.md D20, HEAP.md): break the
        # remembered set six ways, object sizes three, the nursery reserves four,
        # path + once, string coercion twice and SHA-256 three times, rebuild the
        # VM with ORCA/C each time, and require the
        # collector fixtures (scripts/mutation-vm.sh) to catch every one.
        mutation-vm = pkgs.runCommandCC "check-mutation-vm"
          { nativeBuildInputs = [ pkgs.verilator pkgs.python3 ours.orca-toolchain ours.nbc ]; }
          ''
            cp -r ${self}/rtl ${self}/sim ${self}/tests ${self}/target ${self}/scripts .
            chmod -R +w .
            patchShebangs scripts
            verilator --cc --exe --build -j $NIX_BUILD_CORES \
              -Irtl --top-module a2nix_card \
              -Wall -Wno-DECLFILENAME \
              --Mdir build -o a2min \
              rtl/a2nix_card.v sim/a2min.cpp
            ./scripts/mutation-vm.sh ./build/a2min nbc | tee "$out"
            grep -q "MUTATION PASS" "$out"
          '';

        # The VM's C linted on the host (scripts/c-lint.sh): clang -Wall
        # -Wextra -Werror through a shim for ORCA/C's two extensions, and
        # gcsafe, the GC/view rule as a lint. Self-tested by planting (D14).
        c-lint = pkgs.runCommand "check-c-lint"
          { nativeBuildInputs = [ (pkgs.python3.withPackages (ps: [ ps.libclang ]))
                                  pkgs.llvmPackages.clang-unwrapped ]; }
          ''
            cp -r ${self}/target ${self}/scripts .
            chmod -R +w .
            patchShebangs scripts
            ./scripts/c-lint.sh > lint.txt || { cat lint.txt; exit 1; }
            cat lint.txt
            grep -q "^C LINT OK" lint.txt
            cp lint.txt "$out"
          '';

        # Where the VM's code lands (scripts/bank-gate.py): bank 0's size is
        # exactly the committed target/vm/bank0.txt, only allowlisted runtime
        # routines are linked into it, and no symbol is in a bank the VM uses
        # for data. Then the same rules, each broken on purpose (D14).
        bank-gate = pkgs.runCommand "check-bank-gate"
          { nativeBuildInputs = [ pkgs.python3 ours.orca-toolchain ]; }
          ''
            cp -r ${self}/target ${self}/scripts .
            chmod -R +w .
            python3 scripts/bank-gate.py ${ours.cvm}/cvm.bin ${ours.cvm}/cvm.map \
              target/vm/bank0.txt > gate.txt || { cat gate.txt; exit 1; }
            python3 scripts/bank-gate.py --self-test ${ours.cvm}/cvm.bin ${ours.cvm}/cvm.map \
              target/vm/bank0.txt >> gate.txt || { cat gate.txt; exit 1; }
            cat gate.txt
            grep -q "^BANK GATE SELF-TEST OK" gate.txt
            cp gate.txt "$out"
          '';

        # ORCA/C's lint (vm.h `#pragma lint -1`) and the memory map's
        # _Static_asserts stop the build -- proven by planting a violation of
        # each in a copy (scripts/orca-lint-selftest.sh, D14).
        orca-lint = pkgs.runCommand "check-orca-lint"
          { nativeBuildInputs = [ ours.orca-toolchain ]; }
          ''
            cp -r ${self}/target ${self}/scripts .
            chmod -R +w .
            patchShebangs scripts
            ./scripts/orca-lint-selftest.sh | tee "$out"
            grep -q "^ORCA LINT OK" "$out"
          '';

        # D18: the C toolchain end to end. ORCA/C compiles target/tests/smoke.c under
        # orcarun, the ORCA linker links it, omf2bin flattens it, and a2min runs the
        # result: the bytes it leaves in the result area are checked against values
        # computed by hand. A compiler that produced wrong-but-plausible code would
        # fail here, not in the VM port built on it.
        orcac-smoke = pkgs.runCommandCC "check-orcac-smoke"
          { nativeBuildInputs = [ pkgs.verilator pkgs.python3 ]; }
          ''
            cp -r ${self}/rtl ${self}/sim .
            chmod -R +w .
            verilator --cc --exe --build -j $NIX_BUILD_CORES \
              -Irtl --top-module a2nix_card \
              -Wall -Wno-DECLFILENAME \
              --Mdir build -o a2min \
              rtl/a2nix_card.v sim/a2min.cpp
            smoke=${ours.mkOrcaBinary { name = "smoke"; srcs = [ ./target/tests/smoke.c ]; }}
            ./build/a2min --run-bin "$smoke/smoke.bin" | tee "$out"
            grep -q '^status: \$01$' "$out"
            # 42 = $002A at $0300; 12345 * 6789 = 83810205 = $04FED79D at $0302
            grep -q '^result: 2A 00 9D D7 FE 04 ' "$out"
            echo "ORCAC SMOKE PASS: C compiled by ORCA/C ran on a2min" | tee -a "$out"
          '';

        # The harness itself builds and its usage works; the package is what the
        # devShell and mkOrcaBinary use.
        orcarun = ours.orcarun;

        # Regressions for the silent-corruption bug class. Every bug found in this
        # card produced a plausible-looking wrong answer rather than a failure, so
        # each one gets a permanent named test (sim/regress_main.cpp).
        regress = pkgs.runCommandCC "check-regress"
          { nativeBuildInputs = [ pkgs.verilator pkgs.python3 ]; }
          ''
            cp -r ${self}/rtl ${self}/sim ${self}/tests .
            chmod -R +w .
            verilator --cc --exe --build -j $NIX_BUILD_CORES \
              -Irtl --top-module a2nix_card \
              -Wall -Wno-DECLFILENAME \
              --Mdir build -o regress \
              rtl/a2nix_card.v sim/regress_main.cpp
            ./build/regress tests/vectors/l1.txt | tee "$out"
            grep -q "REGRESS PASS" "$out"
          '';

        # Prove the regression suite can actually FAIL, by reintroducing each fixed
        # bug and asserting it is caught. Without this, the suite above could rot into
        # decoration -- which already happened once: R4's first two mutations both
        # SURVIVED, because the FIFO had made the original hazard unreachable.
        mutation = pkgs.runCommandCC "check-mutation"
          { nativeBuildInputs = [ pkgs.verilator pkgs.python3 ]; }
          ''
            cp -r ${self}/rtl ${self}/sim ${self}/tests ${self}/scripts .
            chmod -R +w .
            patchShebangs scripts
            ./scripts/mutation-test.sh | tee "$out"
            grep -q "MUTATION PASS" "$out"
          '';

        # The 65816 side must actually assemble, and its hardcoded fingerprint must
        # still agree with the golden vectors -- otherwise the demo would print a
        # plausible-looking store path while asserting the wrong expected value.
        target-asm = pkgs.runCommand "check-target-asm"
          { nativeBuildInputs = [ pkgs.cc65 pkgs.python3 ]; }
          ''
            cp -r ${self}/target ${self}/tests ${self}/scripts .
            chmod -R +w .
            mkdir -p target/build
            ca65 --cpu 65816 -o target/build/storepath_demo.o \
              -l target/build/storepath_demo.lst target/tests/storepath_demo.s
            ld65 -C target/tests/apple2.cfg -o target/build/storepath_demo.bin \
              -m target/build/storepath_demo.map target/build/storepath_demo.o
            python3 scripts/check-demo-vector.py | tee "$out"
            echo "binary: $(stat -c %s target/build/storepath_demo.bin) bytes" >> "$out"
          '';

        # Lint must stay clean: the RTL is the single source of truth for the MAME
        # device and the bitstream, so warnings here become bugs in two places.
        rtl-lint = pkgs.runCommand "check-rtl-lint"
          { nativeBuildInputs = [ pkgs.verilator ]; }
          ''
            cp -r ${self}/rtl .
            chmod -R +w rtl
            # Lint each module as its own top. Linting them together would trip
            # MULTITOP, and per-module linting is stricter anyway.
            for top in ${nixpkgs.lib.concatStringsSep " " rtlTops}; do
              echo "=== lint $top ==="
              verilator --lint-only -Wall -Wno-DECLFILENAME \
                --top-module "$top" ${rtlSources}
            done
            echo "rtl lint clean" > "$out"
          '';

        # Synthesis smoke test, so the FPGA path never silently rots while all the
        # attention is on simulation. Targets ECP5 (DECISIONS.md D10).
        rtl-synth = pkgs.runCommand "check-rtl-synth"
          { nativeBuildInputs = [ pkgs.yosys ]; }
          ''
            cp -r ${self}/rtl .
            chmod -R +w rtl
            for top in sha256 storepath; do
              echo "=== synth_ecp5 -top $top ==="
              yosys -p "read_verilog ${rtlSources}; synth_ecp5 -top $top; stat" \
                | tee -a "$out"
            done
          '';
      });

      packages = forAll ({ ours, ... }: {
        # Apple II disk tooling, neither of which is in nixpkgs.
        inherit (ours) applecommander cadius;
        # The RTL Verilated into a static library MAME links, and MAME with the card
        # registered as an Apple IIgs slot option (long one-off build, then cached).
        inherit (ours) verilated-card mame-a2nix;
        # The test-only fixture generator (D7/D16): `nix run .#nbc -- in.nix -o out.nbc`.
        inherit (ours) nbc;
        # The C toolchain (D18): ORCA/C's release, the harness, and the wrapper
        # commands. `nix build .#orcac` fetches the release for this machine.
        inherit (ours) orcac orcarun orca-toolchain;
        # The VM (docs/VM.md): `nix build .#cvm` -> result/cvm.bin for a2min --run-nbc.
        inherit (ours) cvm;
        default = ours.verilated-card;
      });

      apps = forAll ({ pkgs, system, ... }: {
        # Regenerate tests/vectors/l1.txt from the real nix on this host. Run this
        # (not the check) when adding store-path cases.
        gen-vectors = {
          type = "app";
          program = toString (pkgs.writeShellScript "gen-vectors" ''
            exec ${pkgs.python313}/bin/python3 scripts/gen-vectors.py "$@"
          '');
        };
      });

      formatter = forAll ({ pkgs, ... }: pkgs.nixpkgs-fmt);
    };
}
