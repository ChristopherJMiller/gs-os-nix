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

  # The carrier's netlist, schematic, LPF and ERC report (hw/carrier/top.py).
  carrierBuild = pkgs.runCommand "hw-carrier-build"
    (hwEnv // { nativeBuildInputs = tools; })
    ''
      export HOME=$TMPDIR
      cp -r ${self}/hw ${self}/rtl .
      chmod -R +w .
      python3 -m hw.carrier.top --out "$out"
    '';

  # The JLCPCB order package and the review artefacts (hw/tools/fab.py): gerbers, BOM, CPL,
  # interactive BOM, renders, layer SVGs and the schematic PDF.
  carrierFab = pkgs.runCommand "hw-carrier-fab"
    (hwEnv // { nativeBuildInputs = tools; })
    ''
      export HOME=$TMPDIR
      cp -r ${self}/hw ${self}/rtl .
      chmod -R +w .
      python3 -m hw.tools.fab --board hw/carrier/carrier.kicad_pcb \
        --sch ${carrierBuild}/carrier.kicad_sch --out "$out"
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
    # cannot see), on the section alone and on the whole carrier, where the module's
    # power rules (J2's 5V pins, the series diode D1) apply too.
    hw-intent = hwCheck "intent" ''
      mkdir -p build/hw
      python3 -m hw.src.slot_bus --netlist build/hw/slot_bus.net > /dev/null
      python3 hw/tools/check_intent.py --selftest build/hw/slot_bus.net
      python3 hw/tools/check_intent.py build/hw/slot_bus.net
      python3 hw/tools/check_intent.py --module hw/carrier/icesugar_pro_v1_3.toml \
        --selftest ${carrierBuild}/carrier.net
      python3 hw/tools/check_intent.py --module hw/carrier/icesugar_pro_v1_3.toml \
        ${carrierBuild}/carrier.net
    '' [ "^HW INTENT SELFTEST OK" "^HW INTENT OK" ];

    # SKiDL's electrical rules check on the carrier, and its planted second driver.
    hw-erc = hwCheck "erc" ''
      python3 hw/tools/check_erc.py --selftest
      python3 hw/tools/check_erc.py
    '' [ "^HW ERC SELFTEST OK" "^HW ERC OK" ];

    # pins.toml <-> carrier_top's ports <-> the carrier netlist <-> the LPF <-> Trellis;
    # the stub must also lint clean.
    hw-pinmap = hwCheck "pinmap" ''
      verilator --lint-only -Wall rtl/board/carrier_top.v && echo 'carrier_top lint clean'
      args="--pins hw/carrier/pins.toml --module hw/carrier/icesugar_pro_v1_3.toml \
        --netlist ${carrierBuild}/carrier.net --verilog rtl/board/carrier_top.v \
        --lpf ${carrierBuild}/carrier.lpf"
      python3 hw/tools/check_pinmap.py --selftest $args
      python3 hw/tools/check_pinmap.py $args
    '' [ "^carrier_top lint clean" "^HW PINMAP SELFTEST OK" "^HW PINMAP OK" ];

    # Review Focus 1, on the placed board: outline, finger orientation and the bevel.
    hw-edge = hwCheck "edge" ''
      python3 hw/tools/check_edge.py --selftest hw/carrier/carrier.kicad_pcb
      python3 hw/tools/check_edge.py hw/carrier/carrier.kicad_pcb
    '' [ "^HW EDGE SELFTEST OK" "^HW EDGE OK" ];

    # The routed board: KiCad's DRC (no violations, nothing unconnected) and, from the
    # carrier netlist, that every footprint and pad net is the netlist's.
    hw-drc = hwCheck "drc" ''
      python3 hw/tools/check_drc.py --selftest --netlist ${carrierBuild}/carrier.net \
        hw/carrier/carrier.kicad_pcb
      python3 hw/tools/check_drc.py --netlist ${carrierBuild}/carrier.net \
        hw/carrier/carrier.kicad_pcb
    '' [ "^HW DRC SELFTEST OK" "^HW DRC OK" ];

    # Every part that is ordered has an LCSC number (J1, the edge fingers, is the one
    # exemption), on the board and in the netlist.
    hw-bom = hwCheck "bom" ''
      python3 hw/tools/check_bom.py --selftest --netlist ${carrierBuild}/carrier.net \
        hw/carrier/carrier.kicad_pcb
      python3 hw/tools/check_bom.py --netlist ${carrierBuild}/carrier.net \
        hw/carrier/carrier.kicad_pcb
    '' [ "^HW BOM SELFTEST OK" "^HW BOM OK" ];

    # The order package's contents: gerbers, drill, one CPL row per machine-placed part.
    hw-fab = hwCheck "fab" ''
      python3 hw/tools/check_fab.py --selftest --board hw/carrier/carrier.kicad_pcb \
        --sch ${carrierBuild}/carrier.kicad_sch
      python3 hw/tools/check_fab.py --board hw/carrier/carrier.kicad_pcb ${carrierFab}
    '' [ "^HW FAB SELFTEST OK" "^HW FAB OK" ];
  };

  packages =lib.optionalAttrs pkgs.stdenv.hostPlatform.isLinux {
    hw-carrier-build = carrierBuild;
    hw-carrier-fab = carrierFab;
  };
}
