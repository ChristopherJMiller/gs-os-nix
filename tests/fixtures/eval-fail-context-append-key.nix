# appendContext: "context key ... is not a store path" -- not in the store directory.
let b = derivation { name = "b"; system = "s"; builder = "/bin/sh"; }; in builtins.appendContext "x" { "/tmp/x" = { path = true; }; }
