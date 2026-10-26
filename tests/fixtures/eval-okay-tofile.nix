# builtins.toFile (DERIVATION.md "builtins.toFile"): a text store path made
# from the contents' SHA-256 and their references -- the plain paths in their
# context -- as a .drv's own path is made. The result's context is itself, a
# plain source. A closure that reaches it follows its references: `viaf` and
# `viah` depend on derivations whose sources are toFile paths, and so on what
# those reference too. Generated with --read-write-mode (the closures need
# the .drvs in the store).
let
  a = derivation { name = "a"; system = "s"; builder = "/bin/sh"; outputs = [ "out" "dev" ]; };
  f = builtins.toFile "hello.txt" "hello\n";
  g = builtins.toFile "script" "#!/bin/sh\ncat ${f}\n";            # references f
  h = builtins.toFile "h" (builtins.unsafeDiscardOutputDependency a.drvPath);   # references a.drv, opaquely
  empty = builtins.toFile "empty" "";
  usesg = derivation { name = "usesg"; system = "s"; builder = "/bin/sh"; src = g; };
  usesh = derivation { name = "usesh"; system = "s"; builder = "/bin/sh"; src = h; };
  viag = derivation { name = "viag"; system = "s"; builder = "/bin/sh"; x = usesg.drvPath; };
  viah = derivation { name = "viah"; system = "s"; builder = "/bin/sh"; x = usesh.drvPath; };
in [
  f g h empty
  (builtins.getContext g)
  (builtins.toFile "same" "hello\n" == f)                            # another name, another path
  usesg.drvPath usesg.outPath usesh.drvPath
  viag.drvPath viah.drvPath
]
