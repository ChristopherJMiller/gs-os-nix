# SRI is always Base64, so a short one is not another base: "invalid SRI hash".
(derivation { name = "f"; system = "s"; builder = "b"; outputHash = "sha256-47DEQpj8HBSa"; }).outPath
