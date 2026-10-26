# addDrvOutputDependencies: a plain path that is not a .drv "is not a derivation".
let b = derivation { name = "b"; system = "s"; builder = "/bin/sh"; }; in builtins.addDrvOutputDependencies (builtins.appendContext "x" { "/nix/store/00000000000000000000000000000000-src" = { path = true; }; })
