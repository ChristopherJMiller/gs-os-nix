# "multiple outputs are not supported in fixed-output derivations". "src" sorts
# after "out", so the first output IS "out" and only the count can catch it.
(derivation { name = "f"; system = "s"; builder = "b"; outputHash = "sha256-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU="; outputs = [ "out" "src" ]; }).outPath
