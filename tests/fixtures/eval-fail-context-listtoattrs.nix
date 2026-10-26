# listToAttrs: a name may not carry a context.
let b = derivation { name = "b"; system = "s"; builder = "/bin/sh"; }; in builtins.listToAttrs [ { name = "${b}"; value = 1; } ]
