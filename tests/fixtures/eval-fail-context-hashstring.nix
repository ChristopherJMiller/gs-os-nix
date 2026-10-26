# hashString: the type may not carry a context -- even an empty substring keeps one.
let b = derivation { name = "b"; system = "s"; builder = "/bin/sh"; }; in builtins.hashString "sha256${builtins.substring 0 0 "${b}"}" "x"
