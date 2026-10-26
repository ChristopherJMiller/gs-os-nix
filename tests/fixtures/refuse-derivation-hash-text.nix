# outputHashMode = "text" is Nix behind the dynamic-derivations experimental
# feature (the host, without it, rejects this): a text-hashed output, whose
# path is computed another way. Refused rather than hashed as flat.
(derivation { name = "f"; system = "s"; builder = "b"; outputHash = "sha256-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU="; outputHashMode = "text"; }).outPath
