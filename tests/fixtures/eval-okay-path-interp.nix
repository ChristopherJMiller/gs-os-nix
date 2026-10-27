# Paths with interpolations -- Nix's ExprConcatStrings with a path first --
# and __curPos. A relative path is compared with a literal one, so the
# answers hold wherever the file is.
let foo = "foo"; d = "a/../b"; in [
  (./${foo} == ./foo)                 # the first segment keeps its '/'
  (./a-${foo}-b == ./a-foo-b)
  (./bar/${foo} == ./bar/foo)
  (./${foo + "/bar"} == ./foo/bar)
  /${foo}
  /${foo}/${"bar"}
  /x/${d}                             # the joined text made canonical, once
  /${"a//b/"}
  /x${foo}                            # no '/' where the literal has none
  (builtins.typeOf /${foo})
  /${/abs/path}                       # a path part is its text, not copied
  (let p = __curPos; in [ p.line p.column (baseNameOf p.file) ])
]
