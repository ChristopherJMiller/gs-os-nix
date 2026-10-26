# import and scopedImport as values (D26): primops, which force their
# arguments as Nix does -- scopedImport's scope a set, first -- before the
# file they would read. An import the generator resolves itself is a literal
# path; anything else is the primop, at run time.
[
  (builtins.typeOf import)
  (builtins.typeOf builtins.import)
  (builtins.typeOf scopedImport)
  (builtins.tryEval (import (throw "the path")))
  (builtins.tryEval (scopedImport (throw "the scope") ./eval-okay-import-values.nix))
  (builtins.tryEval (scopedImport { } (throw "the path")))
  (map import [ ])
  (builtins.length [ import import ])
]
