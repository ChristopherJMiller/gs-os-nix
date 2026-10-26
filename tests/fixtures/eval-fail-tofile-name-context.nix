# toFile: the name may not carry a context (forceStringNoCtx).
let a = derivation { name = "a"; system = "s"; builder = "/bin/sh"; }; in builtins.toFile "x${builtins.substring 0 0 "${a}"}" "x"
