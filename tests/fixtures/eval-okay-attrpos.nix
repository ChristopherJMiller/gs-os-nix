# builtins.unsafeGetAttrPos: every set carries its attributes' positions as
# Nix's do (HEAP.md "ATTRS is struct-of-arrays"). The file is compared by its base name.
let
  pos = n: s: let p = builtins.unsafeGetAttrPos n s; in if p == null then null else [ p.line p.column (baseNameOf p.file) ];
  x = 1;
  s1 = { a = 1; bb = 2; };
  s2 = { a.b.c = 1; a.d = 2; };
  s3 = { inherit x; };
  s4 = { inherit (s1) a; };
  s5 = rec { r = 1; };
  s6 = { "q" = 1; ${"dyn"} = 2; };
  f = { u, v ? 1 }: u;
in [
  (pos "a" s1) (pos "bb" s1)
  (pos "a" s2) (pos "b" s2.a) (pos "c" s2.a.b) (pos "d" s2.a)
  (pos "x" s3) (pos "a" s4) (pos "r" s5)
  (pos "q" s6) (pos "dyn" s6)
  (pos "a" (s1 // { z = 1; })) (pos "z" (s1 // { z = 1; }))
  (pos "a" (builtins.mapAttrs (n: v: v) s1))
  (pos "a" (builtins.removeAttrs s1 [ "bb" ]))
  (pos "a" (builtins.intersectAttrs { a = 0; } s1))
  (pos "k" (builtins.listToAttrs [ { name = "k"; value = 1; } ]))
  (pos "u" (builtins.functionArgs f)) (pos "v" (builtins.functionArgs f))
  (pos "abort" builtins)
  (pos "type" (derivation { name = "d"; builder = "b"; system = "s"; }))
  (pos "name" (derivation { name = "d"; builder = "b"; system = "s"; }))
  (pos "a" (builtins.fromJSON "{\"a\":1}"))
  (pos "success" (builtins.tryEval 1))
  (pos "missing" s1)
  (pos "name" (builtins.parseDrvName "a-1"))
  (pos "a" (builtins.zipAttrsWith (n: v: v) [ s1 ]))
  (pos "a" (s1 // { }))                        # a copy keeps them
  (pos "y" (builtins.mapAttrs (n: v: v) { y = 1; } // { z = 2; }))
  (pos "k" (builtins.listToAttrs [ { value = 2; name = "k"; } { name = "k"; value = 3; } ]))   # the first k, its value's
  (builtins.unsafeGetAttrPos "line" __curPos)  # mkPos's own set has none
  (pos "a" (builtins.groupBy (x: "a") [ 1 ]))
]
