# addDrvOutputDependencies "can only act on derivations, not on a derivation output".
let b = derivation { name = "b"; system = "s"; builder = "/bin/sh"; }; in builtins.addDrvOutputDependencies "${b}"
