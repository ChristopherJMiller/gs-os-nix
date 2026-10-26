# A computed name may not carry context.
let d = derivation { name = "d"; system = "s"; builder = "/b"; }; in { ${d.outPath} = 1; }
