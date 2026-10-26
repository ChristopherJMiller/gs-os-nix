# The string-context builtins (DERIVATION.md "String context"). A context is
# a set of elements -- an output of a derivation, a whole derivation ("=", its
# drvPath), a plain path -- and these read and rewrite it. Each element below is
# decided by one rule; the drvPaths of `uses` show that what they produce is a
# context derivationStrict reads the way Nix does. Read-only (appendContext
# would want its made-up paths in the store); the closure that
# addDrvOutputDependencies restores is in eval-okay-derivation-deep.
let
  a = derivation { name = "a"; system = "s"; builder = "/bin/sh"; outputs = [ "out" "dev" ]; };
  b = derivation { name = "b"; system = "s"; builder = "/bin/sh"; };
  ctx = builtins.getContext;
  opaque = builtins.unsafeDiscardOutputDependency a.drvPath;     # "=a.drv" -> a.drv, a plain path
  uses = name: x: (derivation { inherit name; system = "s"; builder = "/bin/sh"; inherit x; }).drvPath;
in [
  (builtins.hasContext "x") (builtins.hasContext "${b}") (builtins.hasContext (builtins.substring 0 0 "${b}"))
  (builtins.unsafeDiscardStringContext a)                     # coerced first: a set through outPath
  (builtins.hasContext (builtins.unsafeDiscardStringContext "${a}${b.drvPath}"))
  (builtins.unsafeDiscardStringContext "${b}" == "${b}")      # equality ignores context
  (ctx "x")
  (ctx "${a}${a.dev}${b.drvPath}${opaque}")
  (ctx opaque) (ctx (builtins.unsafeDiscardOutputDependency "${a.drvPath}${b}"))
  (ctx (builtins.addDrvOutputDependencies opaque))
  (ctx (builtins.addDrvOutputDependencies b.drvPath))         # already whole: kept
  (ctx (builtins.appendContext "x" {
    "/nix/store/zk00i9dmrkzkyw50mcxmq9v75lq04br0-a.drv" = { outputs = [ "out" ]; path = true; allOutputs = true; };
    "/nix/store/00000000000000000000000000000000-src" = { path = true; };
    "/nix/store/11111111111111111111111111111111-unused" = { path = false; outputs = [ ]; allOutputs = false; };
  }))
  (ctx (builtins.appendContext "${b}" { }))
  (ctx (builtins.appendContext "${b}" { "/nix/store/00000000000000000000000000000000-src" = { path = true; }; }))   # its own and the new
  (builtins.appendContext "x${b}" { "/nix/store/00000000000000000000000000000000-src" = { path = true; }; })
  (uses "opaque" opaque)                                      # a .drv as a plain source: no closure
  (uses "appended" (builtins.appendContext "x" { "/nix/store/fqcx248rkn7bm04smm78xab27aikssah-b.drv" = { outputs = [ "out" ]; }; }))
]
