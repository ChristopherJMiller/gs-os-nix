# A bare hash and no outputHashAlgo: "does not include a type"
(derivation { name = "f"; system = "s"; builder = "b"; outputHash = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"; }).outPath
