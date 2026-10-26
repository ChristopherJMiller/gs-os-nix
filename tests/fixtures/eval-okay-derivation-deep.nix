# A drvPath in a string ("=<drv>" context; DERIVATION.md "A .drv in a
# string"): the derivation depends on the .drv's whole closure -- every .drv
# in it with ALL its outputs, and every path in it as a source, the .drv
# files included. Each element below is decided by one rule: `b` uses only
# a.out, but anything that names b's drvPath gets both of a's outputs; `c`
# reaches `a` two levels down; `top` reaches it through c's own deep inputs;
# `mixed` names one output of `a` the ordinary way as well; `fixed` puts a
# fixed-output derivation in a closure. Generated with
#   nix-instantiate --eval --strict --read-write-mode
# because a read-only evaluation cannot compute a closure of a .drv that is not
# in the store yet ("path ... is not valid").
let
  a = derivation { name = "a"; system = "s"; builder = "/bin/sh"; outputs = [ "out" "dev" ]; };
  b = derivation { name = "b"; system = "s"; builder = "${a}/bin/sh"; };
  leaf = derivation { name = "leaf"; system = "s"; builder = "/bin/sh"; x = a.drvPath; };
  c = derivation { name = "c"; system = "s"; builder = "/bin/sh"; x = b.drvPath; };
  top = derivation { name = "top"; system = "s"; builder = "/bin/sh"; args = [ "${c.drvPath}" ]; };
  mixed = derivation { name = "mixed"; system = "s"; builder = "${a.dev}/bin/sh"; x = "${b.drvPath}:${a}"; };
  fixed = derivation { name = "fixed"; system = "s"; builder = "${a}/bin/fetch";
    outputHash = "sha256-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU="; };
  onfixed = derivation { name = "onfixed"; system = "s"; builder = "/bin/sh"; x = fixed.drvPath; };
  usesc = derivation { name = "usesc"; system = "s"; builder = "${c}/bin/sh"; };
  # addDrvOutputDependencies turns a plain .drv path back into "=<drv>", so the
  # closure returns (eval-okay-context-builtins has the plain one).
  added = derivation { name = "added"; system = "s"; builder = "/bin/sh";
    x = builtins.addDrvOutputDependencies (builtins.unsafeDiscardOutputDependency b.drvPath); };
in [
  leaf.drvPath leaf.outPath
  c.drvPath c.outPath
  top.drvPath
  mixed.drvPath
  onfixed.drvPath onfixed.outPath
  usesc.drvPath usesc.outPath
  added.drvPath
]
