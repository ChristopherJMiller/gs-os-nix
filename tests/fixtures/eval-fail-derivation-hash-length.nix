# "hash 'abcd' has wrong length for hash algorithm 'sha256'"
(derivation { name = "f"; system = "s"; builder = "b"; outputHash = "abcd"; outputHashAlgo = "sha256"; }).outPath
