# A selection key may not carry context.
let d = derivation { name = "d"; system = "s"; builder = "/b"; }; in { a = 1; }.${builtins.substring 0 0 d.outPath + "a"}
