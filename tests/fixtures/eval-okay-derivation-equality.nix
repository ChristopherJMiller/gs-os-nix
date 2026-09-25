# Nix's eqValues: two sets that are both derivations (`type` forces to
# "derivation") and both have an outPath are equal iff the outPaths are,
# whatever else they hold. Anything short of that compares as sets.
let
  drv = extra: { type = "derivation"; outPath = "/p"; } // extra;
in [
  (drv { a = 1; } == drv { b = 2; })
  (drv { } == drv { outPath = "/q"; })
  ({ type = "derivation"; a = 1; } == { type = "derivation"; b = 1; })
  ({ type = "package"; outPath = "/p"; a = 1; } == { type = "package"; outPath = "/p"; })
  ({ type = "derivation"; outPath = "/p"; } == { type = "package"; outPath = "/p"; })
  (drv { } == drv { })
  ({ a = { type = "derivation"; outPath = "/p"; x = 1; }; } == { a = drv { }; })
]
