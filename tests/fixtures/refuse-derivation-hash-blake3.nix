# blake3 is Nix behind the blake3-hashes experimental feature (the host, without
# it, rejects this). Refused rather than read as an unknown algorithm, which
# Nix drops -- and then this hash would have no type at all (E_TYPE).
(derivation { name = "f"; system = "s"; builder = "b"; outputHash = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"; outputHashAlgo = "blake3"; }).outPath
