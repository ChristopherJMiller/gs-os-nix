# A leaf derivation -- a builder, arguments and an environment, no inputs --
# whose .drv path and output path must be the host's to the byte: M4's shape
# (docs/DERIVATION.md). The rest is derivation.nix's wrapper, which the
# generator embeds verbatim, and Nix's rule that two derivations are equal iff
# their outPaths are. One derivation: a second is E_UNSUP until string context.
let
  d = derivation {
    name = "hello";
    system = "x86_64-linux";
    builder = "/bin/sh";
    args = [ "-c" "echo hi > $out" ];
  };
in [
  d.drvPath
  d.outPath
  "${d}"
  d.type
  d.name
  d.outputName
  (builtins.attrNames d)
  (builtins.length d.all)
  d.drvAttrs.builder
  (d.out.outPath == d.outPath)
  (d == d.out)
  (d == d // { extra = 1; })
  (builtins.typeOf d)
]
