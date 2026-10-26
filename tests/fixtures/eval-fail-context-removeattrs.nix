# removeAttrs: each name may not carry a context, even one that is not in the set.
let b = derivation { name = "b"; system = "s"; builder = "/bin/sh"; }; in builtins.removeAttrs { } [ "${b}" ]
