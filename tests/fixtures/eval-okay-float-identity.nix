# Which floats are the same value (docs/FLOAT.md): a NaN equals nothing, but
# Nix compares list elements and attribute values by pointer first, so
# [ nan ] == [ nan ] is true exactly when both name one Value. Each element
# is one way of naming it -- an alias, an argument, an attribute, a
# selection, a fresh computation -- answered by the host. The VM copies a
# forced float into its thunk, so these say where it must share instead.
let
  inf = 1.0e308 * 10;
  nan = inf - inf;
  y = nan;
  f = v: [ v ];
  s = { a = nan; b = s.a; };
  g = { v }: v;
in [
  ([ nan ] == [ y ]) (f nan == f nan) ([ s.a ] == [ s.b ]) ([ (inf - inf) ] == [ (inf - inf) ])
  ([ (g { v = nan; }) ] == [ nan ]) ({ a = nan; } == { a = y; }) ([ (builtins.head [ nan ]) ] == [ nan ])
  (let z = nan; in [ z ] == [ z ]) ([ (if true then nan else 1) ] == [ nan ]) ([ (nan + 0) ] == [ (nan + 0) ])
  (builtins.elem nan [ nan ]) ([ (let q = nan; in q) ] == [ nan ])
]
