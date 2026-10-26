# Base64 that decodes to 33 bytes is not a sha256.
(derivation { name = "f"; system = "s"; builder = "b"; outputHash = "sha256-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFUA"; }).outPath
