# addDrvOutputDependencies: "context ... must have exactly one element, but has 2".
# Two plain .drv paths, so the first alone would be accepted: only the count refuses.
let
  a = derivation { name = "a"; system = "s"; builder = "/bin/sh"; };
  b = derivation { name = "b"; system = "s"; builder = "/bin/sh"; };
  plain = d: builtins.unsafeDiscardOutputDependency d.drvPath;
in builtins.addDrvOutputDependencies "${plain a}${plain b}"
