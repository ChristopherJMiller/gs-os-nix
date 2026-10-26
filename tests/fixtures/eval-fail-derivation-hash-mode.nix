# outputHashMode is checked even with no outputHash: "invalid value 'foo' for 'outputHashMode' attribute"
(derivation { name = "f"; system = "s"; builder = "b"; outputHashMode = "foo"; }).outPath
