# toString of a list joins its elements with spaces -- except after an element
# that is an empty list, where Nix adds none (its source: "not quite correct").
# Derivation arguments and environment values are coerced the same way.
[
  (builtins.toString [ [ ] "a" [ ] [ ] "b" [ [ ] ] "c" ])
  (builtins.toString [ 1 [ ] 2 ])
  (builtins.toString [ [ ] ])
  (builtins.toString [ [ ] [ ] ])
  (builtins.toString [ "x" [ ] ])
  (builtins.toString [ [ 1 [ ] ] [ ] 3 ])
  (builtins.toString [ true false null "s" [ "n" [ ] ] ])
]
