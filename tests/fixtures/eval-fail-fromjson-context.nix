# fromJSON: the text may not carry context.
let d = derivation { name = "d"; system = "s"; builder = "/b"; }; in builtins.fromJSON d.outPath
