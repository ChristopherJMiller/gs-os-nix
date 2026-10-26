# appendContext: allOutputs of a path that is not a .drv.
let b = derivation { name = "b"; system = "s"; builder = "/bin/sh"; }; in builtins.appendContext "x" { "/nix/store/00000000000000000000000000000000-src" = { allOutputs = true; }; }
