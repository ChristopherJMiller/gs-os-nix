# A derivation that uses another's output: "${a}" carries a's context, which
# makes a an input of b -- inputDrvs, and the "text:<a.drv>" reference in b's
# own path. Until string context this was refused, because without it b's .drv
# would have been well-formed and wrong (DERIVATION.md, D22).
let
  a = derivation { name = "a"; system = "s"; builder = "b"; };
  b = derivation { name = "b"; system = "s"; builder = "${a}/bin/sh"; };
in b.drvPath
