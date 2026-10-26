# hasContext wants a string, not a path (it is not a coercion).
let b = derivation { name = "b"; system = "s"; builder = "/bin/sh"; }; in builtins.hasContext /nix/store
