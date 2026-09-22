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
      });

      forAll = f: nixpkgs.lib.genAttrs systems (system:
        let pkgs = nixpkgs.legacyPackages.${system};
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
            (python313.withPackages (ps: with ps; [ cocotb pytest ]))

            # --- 65816 target ---
            cc65                 # ca65 --cpu 65816; the C compiler is 6502-only

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

        # The opcode table lives in BYTECODE.md, opcodes.rs and opcodes.inc. A
        # disagreement between generator and VM would yield fixtures that run and
        # return plausible wrong answers, so the three are diffed mechanically.
        nbc-opcodes = pkgs.runCommand "check-nbc-opcodes"
          { nativeBuildInputs = [ pkgs.python3 ]; }
          ''
            cp -r ${self}/docs ${self}/tools ${self}/target ${self}/scripts .
            chmod -R +w .
            python3 scripts/check-opcodes.py --selftest | tee "$out"
            python3 scripts/check-opcodes.py | tee -a "$out"
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
          { nativeBuildInputs = [ pkgs.verilator pkgs.python3 pkgs.cc65 ours.nbc ]; }
          ''
            cp -r ${self}/rtl ${self}/sim ${self}/tests ${self}/target ${self}/scripts ${self}/tools .
            chmod -R +w .
            patchShebangs scripts
            mkdir -p target/build
            ca65 --cpu 65816 -I target/vm -o target/build/vm.o target/vm/vm.s
            ld65 -C target/vm/vm.cfg -o target/build/vm.bin target/build/vm.o
            verilator --cc --exe --build -j $NIX_BUILD_CORES \
              -Irtl --top-module a2nix_card \
              -Wall -Wno-DECLFILENAME \
              --Mdir build -o a2min \
              rtl/a2nix_card.v sim/a2min.cpp
            python3 scripts/scoreboard.py \
              --nbc nbc --a2min ./build/a2min --vm target/build/vm.bin \
              --corpus tests/corpus/tvix --corpus tests/fixtures \
              --build target/build/nbc \
              --expected tests/corpus/expected-pass.txt | tee "$out"
            grep -q "^SCOREBOARD:" "$out"
            ./scripts/fixture-purity.sh target/build/nbc | tee -a "$out"

            # The collector test: every local fixture again, with a nursery
            # collection at every bytecode boundary. A fixture that passes
            # normally and fails here has a root the collector cannot see.
            python3 scripts/scoreboard.py \
              --nbc nbc --a2min ./build/a2min --vm target/build/vm.bin \
              --corpus tests/fixtures --build build/gc \
              --expected tests/corpus/expected-pass.txt --gc-stress \
              | tee -a "$out" | grep -q "^SCOREBOARD: [0-9]* / [0-9]* pass (0 fail, 0 error"

            # The ratchet must be able to fail: a listed test that no longer
            # passes is a regression, and the run must say so and exit non-zero.
            # Plant one by giving a real fixture the wrong expected value.
            mkdir -p ghost && cp tests/fixtures/eval-okay-let-forward.* ghost/
            echo "wrong" > ghost/eval-okay-let-forward.exp
            echo "ghost/eval-okay-let-forward" > ghost.txt
            if python3 scripts/scoreboard.py \
                 --nbc nbc --a2min ./build/a2min --vm target/build/vm.bin \
                 --corpus ghost --build build/ghost --expected ghost.txt \
                 > ghost.log 2>&1; then
              echo "scoreboard ratchet SURVIVED a planted regression" >&2; exit 1
            fi
            grep -q "^REGRESSION:" ghost.log
            echo "ratchet self-test: a planted regression is caught" | tee -a "$out"
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
