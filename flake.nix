{
  description = "Nix on an Apple IIgs, with a verb-based FPGA accelerator card";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];
      forAll = f: nixpkgs.lib.genAttrs systems (system: f {
        inherit system;
        pkgs = nixpkgs.legacyPackages.${system};
      });

      # Every RTL module, in dependency order.
      rtlSources = "rtl/sha256_core.v rtl/sha256.v rtl/storepath.v rtl/a2nix_card.v";
      rtlTops = [ "sha256_core" "sha256" "storepath" "a2nix_card" ];
    in
    {
      devShells = forAll ({ pkgs, ... }: {
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

            # --- Emulation ---
            mame                 # patched variant will live in nix/pkgs at L2

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

      checks = forAll ({ pkgs, ... }: {

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

      packages = forAll ({ pkgs, ... }: rec {
        # The RTL, Verilated into a static library MAME can link. Keeps the RTL as
        # the single source of truth for both MAME and a real bitstream.
        verilated-card = pkgs.callPackage ./nix/pkgs/verilated-card.nix {
          rtlSrc = ./rtl;
        };

        # MAME with the card registered as an Apple IIgs slot option.
        # Long one-off build; cached afterwards.
        mame-a2nix = pkgs.callPackage ./nix/pkgs/mame-a2nix.nix {
          verilatedCard = verilated-card;
          cardSrc = ./mame;
        };

        default = verilated-card;
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
