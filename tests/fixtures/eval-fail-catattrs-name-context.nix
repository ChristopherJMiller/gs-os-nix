# catAttrs's name may not carry context.
let d = derivation { name = "d"; system = "s"; builder = "/b"; }; in builtins.catAttrs d.outPath [ ]
