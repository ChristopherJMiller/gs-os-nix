# Corners of builtins and the parser, each element one rule, answered by the
# host: one builtins set, holding itself; a name that is no builtin is a
# missing attribute, lazily; the value builtins; toPath; URI literals; and
# merging into a rec set, whose rec-ness is the first definition's.
let
  a = 1;
  alias = builtins;
in [
  (builtins ? fnord) (if builtins ? fnord then builtins.fnord "x" else "no") (builtins.fnord or 5)
  (builtins.length [ builtins.fnord ])
  (builtins ? builtins) (builtins.builtins == builtins) ([ builtins ] == [ builtins.builtins ])
  (alias == builtins.builtins) (builtins.builtins.builtins.add 1 2) (with builtins; builtins.add 2 3)
  (builtins.hasAttr "toPath" builtins) (builtins.isAttrs builtins.builtins)
  builtins.nixVersion builtins.langVersion (builtins.compareVersions "2.18" builtins.nixVersion)
  (builtins.toPath "/a//b/../c") (builtins.toPath "/") (builtins.toPath "/x/./y/") (builtins.typeOf (builtins.toPath "/a"))
  (builtins.toPath { __toString = s: "/s/t"; }) (builtins.toPath /abs/p)
  (builtins.hasContext (builtins.toPath (derivation { name = "d"; system = "s"; builder = "/b"; }).outPath))
  https://x.org/a?b=c (builtins.typeOf https://x.org/a)
  { set = rec { a = 21; }; set = { b = 2 * a; }; }
  { set = { a = 21; }; set = rec { b = 2 * a; }; }
  { set = { b = 2 * a; }; set = rec { a = 21; }; }
  { s = rec { x = 1; y = x + 1; }; s.z = 3; }
]
