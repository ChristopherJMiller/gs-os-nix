# Sets: built by listToAttrs, selected by computed names, merged by //, listed.
let
  names = builtins.genList (i: "k${toString i}") 200;
  s = builtins.listToAttrs (map (n: { name = n; value = builtins.stringLength n; }) names);
  t = s // { extra = 1; k0 = 100; };
  sum = builtins.foldl' (a: n: a + t.${n}) 0 names;
in [ sum (builtins.length (builtins.attrNames t)) (t.k199 + t.extra) ]
