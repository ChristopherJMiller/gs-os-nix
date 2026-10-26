# appendContext: outputs of a path that is not a .drv (an empty list would be fine).
let b = derivation { name = "b"; system = "s"; builder = "/bin/sh"; }; in builtins.appendContext "x" { "/nix/store/00000000000000000000000000000000-src" = { outputs = [ "out" ]; }; }
