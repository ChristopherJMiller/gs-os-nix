# The hash says sha256, outputHashAlgo sha1: "should have type 'sha1'"
(derivation { name = "f"; system = "s"; builder = "b"; outputHash = "sha256-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU="; outputHashAlgo = "sha1"; }).outPath
