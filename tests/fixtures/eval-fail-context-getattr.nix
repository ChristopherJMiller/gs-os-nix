# An attribute name may not carry a context: "is not allowed to refer to a store path".
let b = derivation { name = "b"; system = "s"; builder = "/bin/sh"; }; in builtins.getAttr "${b}" { }
