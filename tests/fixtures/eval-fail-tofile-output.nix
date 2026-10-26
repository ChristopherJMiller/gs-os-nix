# toFile: "files created by builtins.toFile may not reference derivations" -- an output.
let a = derivation { name = "a"; system = "s"; builder = "/bin/sh"; }; in builtins.toFile "x" "${a}"
