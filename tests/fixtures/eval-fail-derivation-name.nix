# A derivation's name must be a store-path name: no spaces.
(derivation { name = "has space"; system = "s"; builder = "b"; }).drvPath
