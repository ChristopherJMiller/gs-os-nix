# One output, but a fixed output must be called "out".
(derivation { name = "f"; system = "s"; builder = "b"; outputHash = "sha256-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU="; outputs = [ "bin" ]; }).outPath
