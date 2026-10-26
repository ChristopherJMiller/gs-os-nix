# appendContext: a hash part with a letter nix32 lacks ("e") is not a store path.
let b = derivation { name = "b"; system = "s"; builder = "/bin/sh"; }; in builtins.appendContext "x" { "/nix/store/eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee-src" = { path = true; }; }
