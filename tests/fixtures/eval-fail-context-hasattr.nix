# An attribute name may not carry a context (hasAttr would otherwise say false).
let b = derivation { name = "b"; system = "s"; builder = "/bin/sh"; }; in builtins.hasAttr "${b}" { }
