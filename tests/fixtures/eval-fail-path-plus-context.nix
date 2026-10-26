# "a string that refers to a store path cannot be appended to a path": a
# string with context does not become part of a path value.
let a = derivation { name = "a"; system = "s"; builder = "b"; };
in /foo + "${a}"
