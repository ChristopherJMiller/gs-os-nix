# A derivation's own attributes are positions in Nix's /derivation-internal.nix,
# a line later than the file's (Nix embeds it after a newline); its
# attributes from the argument are the argument's.
let d = derivation { name = "d"; builder = "b"; system = "s"; }; in [ (builtins.unsafeGetAttrPos "type" d) (builtins.unsafeGetAttrPos "outPath" d) (builtins.unsafeGetAttrPos "drvPath" d) (builtins.unsafeGetAttrPos "out" d) (builtins.unsafeGetAttrPos "all" d) (builtins.unsafeGetAttrPos "outputName" d) (builtins.unsafeGetAttrPos "line" __curPos) ]
