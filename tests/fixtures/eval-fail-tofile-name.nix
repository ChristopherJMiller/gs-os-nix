# toFile: the name must be a store-path name.
let a = derivation { name = "a"; system = "s"; builder = "/bin/sh"; }; in builtins.toFile "a b" "x"
