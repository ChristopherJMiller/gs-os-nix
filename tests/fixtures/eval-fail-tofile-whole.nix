# toFile: nor a whole derivation (a drvPath).
let a = derivation { name = "a"; system = "s"; builder = "/bin/sh"; }; in builtins.toFile "x" a.drvPath
