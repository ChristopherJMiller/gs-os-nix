# compareVersions takes strings without context.
let d = derivation { name = "d"; system = "s"; builder = "/b"; }; in builtins.compareVersions d.outPath "1"
