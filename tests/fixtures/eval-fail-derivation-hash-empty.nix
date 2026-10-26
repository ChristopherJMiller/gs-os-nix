# "empty hash requires explicit hash algorithm"
(derivation { name = "f"; system = "s"; builder = "b"; outputHash = ""; }).outPath
