# Paths as values (BYTECODE.md "Constants", HEAP.md PATH). Literals are made
# absolute and canonical by the generator -- relative ones against /pwd/lang,
# the fixture convention -- and `+` canonicalises again at run time
# (data.c path_join). Long enough paths that a PATH mis-sized by the collector
# (it has no context word, unlike a STRING) would be truncated when promoted,
# which GC stress does to every object. The .exp is real Nix's answer with its
# working and home directories replaced by that convention.
let
  base = /nix/store/some-fairly-long-directory-name;
  rel = ./sub/../file.nix;
  joined = base + "/lib/../bin/./tool";
in [
  joined
  rel
  ~/config
  (builtins.typeOf joined)
  (joined == /nix/store/some-fairly-long-directory-name/bin/tool)
  (joined == "/nix/store/some-fairly-long-directory-name/bin/tool")
  (toString joined)
  (builtins.dirOf joined)
  (builtins.typeOf (builtins.dirOf joined))
  (builtins.baseNameOf joined)
  (base + /extra/path)
  (/a/b/c + "/../../..")
  (/a < /b)
  ([ /x/y ] == [ /x/y ])
]
