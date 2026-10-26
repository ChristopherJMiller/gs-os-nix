# parseDrvName takes a string without context.
let d = derivation { name = "d"; system = "s"; builder = "/b"; }; in builtins.parseDrvName d.outPath
