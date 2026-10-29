# SPDX-License-Identifier: GPL-3.0-or-later
#
# SKiDL 3.0.0 (adds the KICAD10 tool), overriding nixpkgs' 2.2.1. Schematic
# generation now lives in `schematizer`. Left out on purpose: kinet2pcb (only
# imported by SKiDL's own PCB generator, which we do not use -- boards are built
# from the netlist by hw/tools) and inspice (SPICE simulation).
{ lib
, python3Packages
, fetchPypi
, schematizer
}:

python3Packages.skidl.overridePythonAttrs (old: rec {
  version = "3.0.0";
  pyproject = true;
  format = null;

  src = fetchPypi {
    pname = "skidl";
    inherit version;
    hash = "sha256-Jl8rGIqHxOcZCkbgisdujgELACMq71CT9dgB4GKIWw4=";
  };

  build-system = [ python3Packages.setuptools ];
  propagatedBuildInputs = [
    schematizer
    python3Packages.simp-sexp
    python3Packages.ply
    python3Packages.rich
    python3Packages.graphviz
    python3Packages.deprecation
    python3Packages.kinparse
    python3Packages.sexpdata
  ];

  pythonRemoveDeps = [ "kinet2pcb" "inspice" ];
})
