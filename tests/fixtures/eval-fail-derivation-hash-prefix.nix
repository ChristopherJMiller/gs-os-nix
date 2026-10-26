# An unknown algorithm before the colon is an error even with outputHashAlgo set.
(derivation { name = "f"; system = "s"; builder = "b"; outputHash = "foo:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"; outputHashAlgo = "sha256"; }).outPath
