# A group name may not carry context.
let d = derivation { name = "d"; system = "s"; builder = "/b"; }; in builtins.groupBy (x: d.outPath) [ 1 ]
