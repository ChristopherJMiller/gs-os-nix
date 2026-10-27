# M4's work: four chains of derivations, each step an input of the next --
# ATerms, hashDerivationModulo, store paths, string context -- down to the
# .drv paths. Three deep, which the C stack holds with room (VM.md).
let
  step = tag: prev: i: derivation {
    name = "${tag}-${toString i}";
    system = "w65-gsos";
    builder = "/bin/sh";
    args = [ "-c" "echo ${prev} > $out" ];
  };
  chain = tag: builtins.foldl' (step tag) "start" (builtins.genList (i: i) 3);
in map (t: (chain t).drvPath) [ "a" "b" "c" "d" ]
