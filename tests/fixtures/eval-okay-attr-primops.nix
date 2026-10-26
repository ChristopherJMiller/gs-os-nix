# mapAttrs, catAttrs, groupBy, zipAttrsWith, isPath, genericClosure, as Nix
# 2.34 has them (primops.cc), each element one rule, answered by the host.
let
  inf = 1.0e308 * 10;
  nan = inf - inf;
in [
  (builtins.mapAttrs (n: v: n + toString v) { a = 1; b = 2; })
  (builtins.attrNames (builtins.mapAttrs (throw "f is not forced") { a = 1; }))
  ((builtins.mapAttrs (n: v: 1) { a = throw "values are lazy"; }).a)
  (builtins.mapAttrs (n: v: v) { })
  (builtins.catAttrs "a" [ { a = 1; } { b = 2; } { a = 3; } ])
  (builtins.length (builtins.catAttrs "a" [ { a = throw "unforced"; } ]))
  (builtins.catAttrs "a" [ ])
  (builtins.groupBy (x: if x > 2 then "big" else "small") [ 1 2 3 4 1 ])
  (builtins.groupBy (x: x) [ "b" "a" "b" ])
  (builtins.groupBy (x: x) [ ])
  ((builtins.groupBy (x: "k") [ (throw "elements unforced") ]).k != [ ])
  (builtins.zipAttrsWith (n: vs: [ n vs ]) [ { a = 1; b = 2; } { a = 3; } { c = 4; } ])
  (builtins.zipAttrsWith (n: vs: builtins.length vs) [ { a = throw "x"; } { a = 2; } ])
  (builtins.zipAttrsWith (n: vs: vs) [ ])
  (builtins.isPath ./x) (builtins.isPath "/x") (builtins.isPath 1)
  (builtins.genericClosure { startSet = [ { key = 1; } ]; operator = x: if x.key < 4 then [ { key = x.key + 1; } { key = x.key * 2; } ] else [ ]; })
  (builtins.genericClosure { startSet = [ { key = "a"; v = 1; } { key = "a"; v = 2; } ]; operator = x: [ ]; })
  (builtins.genericClosure { startSet = [ { key = [ 1 2 ]; } { key = [ 1 2 ]; } { key = [ 1 3 ]; } ]; operator = x: [ ]; })
  (builtins.genericClosure { startSet = [ { key = 1; } { key = 1.0; } { key = 2.5; } ]; operator = x: [ ]; })
  (builtins.genericClosure { startSet = [ ]; })
  (map (x: x.key) (builtins.genericClosure { startSet = [ { key = 3; } ]; operator = x: if x.key == 0 then [ ] else [ { key = x.key - 1; } { key = x.key; } ]; }))
  (builtins.length (builtins.genericClosure { startSet = [ { key = nan; } { key = 7; } ]; operator = x: [ ]; }))
  (builtins.genericClosure { startSet = [ { key = 1; extra = throw "unforced"; } ]; operator = x: [ ]; } != [ ])
]
