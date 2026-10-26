# toFile: the contents must be a string (forceString, not a coercion).
let a = derivation { name = "a"; system = "s"; builder = "/bin/sh"; }; in builtins.toFile "x" 1
