# mkOrcaBinary { name; srcs; optimize ? true; includes ? []; org ? "0x2000" }
#
# A derivation that compiles C with ORCA/C under orcarun, links it, and converts
# the load file to the flat binary a2min runs: $out/NAME.bin, with $out/NAME.omf
# alongside for inspection. docs/TOOLCHAIN.md.
{ lib
, runCommand
, orca-toolchain
}:

{ name
, srcs
, optimize ? true
, includes ? [ ]
, org ? "0x2000"
}:

runCommand "orca-${name}"
  {
    nativeBuildInputs = [ orca-toolchain ];
    inherit srcs;
  }
  ''
    # Sources come in as store paths whose parent is the store root; orcacc picks
    # up a source's sibling headers, so give them a directory of their own first.
    # In the order given: the load file's entry point is the first function of
    # the first object, so the first source must stay first.
    mkdir -p src "$out"
    list=""
    for s in $srcs; do
      n=$(basename "$s"); cp "$s" "src/''${n#*-}"; list="$list src/''${n#*-}"
    done
    orcabuild -o "$out/${name}.bin" --org ${org} \
      ${lib.optionalString optimize "-O"} \
      ${lib.concatMapStringsSep " " (d: "-I ${d}") includes} \
      $list
  ''
