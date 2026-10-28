# SPDX-License-Identifier: GPL-3.0-or-later
#
# The hardware side (docs/HARDWARE.md): the `.#hw` shell, and the checks and
# packages that grow around it. Linux only; every output is empty elsewhere.
#
# Each check is added through `hwCheck name script expect`: a sandboxed
# runCommand with hw/ and rtl/ copied in and the whole toolchain on PATH. The
# script's output is teed to $out, and each regex in `expect` must match a line
# of it (the repo's check pattern: `grep -q "^... OK" "$out"`).
{ pkgs, self }:

let
  inherit (pkgs) lib;

  schematizer = pkgs.python3Packages.callPackage ./pkgs/schematizer.nix { };
  skidl = pkgs.callPackage ./pkgs/skidl.nix { inherit schematizer; };

  # KiCad's Python bindings (`kicad`) are built for nixpkgs' default python3.
  py = pkgs.python3.withPackages (p: [ p.kicad skidl schematizer p.pytest ]);

  libs = pkgs.kicad-small.passthru.libraries;

  hwEnv = {
    KICAD_SYMBOL_DIR = "${libs.symbols}/share/kicad/symbols";
    KICAD10_SYMBOL_DIR = "${libs.symbols}/share/kicad/symbols";
    KICAD10_FOOTPRINT_DIR = "${libs.footprints}/share/kicad/footprints";
    TRELLIS_DB = "${pkgs.trellis}/share/trellis/database";
  };

  tools = with pkgs; [
    kicad-small
    py
    freerouting
    kikit
    interactive-html-bom
    xvfb-run
    verilator
  ];

  hwCheck = name: script: expect:
    pkgs.runCommand "check-hw-${name}"
      (hwEnv // { nativeBuildInputs = tools; })
      ''
        export HOME=$TMPDIR
        cp -r ${self}/hw ${self}/rtl .
        chmod -R +w .
        {
        ${script}
        } | tee "$out"
        ${lib.concatMapStrings (e: "grep -q ${lib.escapeShellArg e} \"$out\"\n") expect}
      '';
in
{
  # devShells.<system>.hw must exist on every system, so off Linux it is an
  # empty shell rather than a missing attribute.
  devShell =
    if pkgs.stdenv.hostPlatform.isLinux then
      pkgs.mkShell (hwEnv // {
        name = "gs-os-nix-hw";
        packages = tools ++ [ pkgs.just pkgs.trellis ];
        shellHook = ''
          echo "gs-os-nix hw  --  see docs/HARDWARE.md"
          echo "  kicad-cli $(kicad-cli version 2>&1 | head -1)"
        '';
      })
    else
      pkgs.mkShell { name = "gs-os-nix-hw-linux-only"; };

  checks = lib.optionalAttrs pkgs.stdenv.hostPlatform.isLinux {
    hw-tests = hwCheck "tests" ''
      python3 -m pytest -q hw/tests && echo 'HW TESTS OK'
    '' [ "^HW TESTS OK" ];

    # The slot bus section's power-up state, daisy chains and rails (what ERC and DRC
    # cannot see). Task 6 adds the carrier netlist and --module.
    hw-intent = hwCheck "intent" ''
      mkdir -p build/hw
      python3 -m hw.src.slot_bus --netlist build/hw/slot_bus.net > /dev/null
      python3 hw/tools/check_intent.py --selftest build/hw/slot_bus.net
      python3 hw/tools/check_intent.py build/hw/slot_bus.net
    '' [ "^HW INTENT SELFTEST OK" "^HW INTENT OK" ];
  };

  packages = lib.optionalAttrs pkgs.stdenv.hostPlatform.isLinux { };
}
