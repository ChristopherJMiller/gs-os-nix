# The `derivation` builtin, verbatim from Nix 2.34.8 (src/libexpr/primops/
# derivation.nix, LGPL-2.1, doc comment removed). Nix evaluates this file
# around the `derivationStrict` primop; the generator compiles it the same way,
# as an import (docs/DERIVATION.md).
drvAttrs@{
  outputs ? [ "out" ],
  ...
}:

let

  strict = derivationStrict drvAttrs;

  commonAttrs =
    drvAttrs
    // (builtins.listToAttrs outputsList)
    // {
      all = map (x: x.value) outputsList;
      inherit drvAttrs;
    };

  outputToAttrListElement = outputName: {
    name = outputName;
    value = commonAttrs // {
      outPath = builtins.getAttr outputName strict;
      drvPath = strict.drvPath;
      type = "derivation";
      inherit outputName;
    };
  };

  outputsList = map outputToAttrListElement outputs;

in
(builtins.head outputsList).value
