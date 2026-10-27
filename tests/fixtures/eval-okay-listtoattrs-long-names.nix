# listToAttrs inserts each name by a binary search over the names so far.
# One instruction views every name it inserts: 100 promoted names of ~700
# bytes are 70 KB of views, more than the window ring holds, unless it turns
# over between insertions (prims.c) -- so this is the test that it does. (So
# much live text is E_NOMEM with no card, as eval-okay-big-operations is.)
let
  tail = builtins.concatStringsSep "" (builtins.genList (i: "abcdefghij") 70);
  names = builtins.genList (i: "${toString (i * 7919 - i * i)}-${tail}") 100;
  s = builtins.listToAttrs (map (n: { name = n; value = builtins.stringLength n; }) names);
  ks = builtins.attrNames s;
in [ (builtins.length ks) (builtins.substring 0 12 (builtins.head ks)) (builtins.substring 0 12 (builtins.elemAt ks 99))
     s.${builtins.elemAt names 50} ]
