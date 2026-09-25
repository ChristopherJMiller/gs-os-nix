# __ignoreNulls drops null attributes and is not in the environment itself;
# an attribute named like an output is overwritten by the output's path;
# __impure = false is consumed. (docs/DERIVATION.md, measured on the host.)
let
  d = derivation {
    name = "ign";
    system = "s";
    builder = "b";
    __ignoreNulls = true;
    __impure = false;
    gone = null;
    kept = "k";
    out = "overridden";
  };
in [ d.drvPath d.outPath d.out.outPath d.gone d.kept ]
