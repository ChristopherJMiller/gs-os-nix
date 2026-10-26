# String context (docs/DERIVATION.md): a derivation's inputs are the contexts of
# its strings, so each operation that builds a string out of strings must carry
# them. Each `via` derivation below reaches `a` through ONE such operation, and
# its .drv path (the host's, in the .exp) is only right if that operation kept
# a's context. `hashed` reaches it through hashString, which drops context, so
# it has no inputs at all. `c` depends on `b`, which depends on both outputs of
# `a`: the modulo hash of an input that has inputs of its own.
let
  a = derivation { name = "a"; system = "s"; builder = "/bin/sh"; outputs = [ "out" "dev" ]; };
  b = derivation { name = "b"; system = "s"; builder = "${a}/bin/sh"; inc = "${a.dev}/include"; };
  c = derivation { name = "c"; system = "s"; builder = "/bin/sh"; args = [ "${b}" ]; };
  via = name: x: (derivation { inherit name; system = "s"; builder = "/bin/sh"; inherit x; }).drvPath;
in [
  a.drvPath b.drvPath c.drvPath c.outPath
  (via "plus" ("/bin/" + a))
  (via "plus-right" ("x" + "${a.dev}"))
  (via "sep" (builtins.concatStringsSep ":" [ "x" "${a}" "y" ]))
  (via "sep-separator" (builtins.concatStringsSep "${a}" [ "x" "y" ]))
  (via "substring" (builtins.substring 0 10 "${a}"))
  (via "substring-empty" (builtins.substring 99 1 "${a}"))
  (via "basename" (baseNameOf "${a}/bin"))
  (via "dirof" (dirOf "${a}/bin/sh"))
  (via "dirof-dot" (dirOf (baseNameOf "${a}")))
  (via "dirof-slash" (dirOf (builtins.substring 10 3 "${a}")))
  (via "tostring" (toString [ a "y" ]))
  (via "two" "${a}${b}")
  (via "hashed" (builtins.hashString "sha256" "${a}"))
  ("${a}" == a.outPath)
  (builtins.substring 0 11 "${a}")
]
