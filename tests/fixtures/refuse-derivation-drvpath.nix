# A drvPath in a derivation's attributes ("=<drv>" context): Nix adds the whole
# closure of that .drv, which the VM does not keep -- refused rather than
# answered without it (DERIVATION.md "Refused"). Nix itself cannot answer it
# without the .drv in its store: a read-only evaluation says it "is not valid".
let a = derivation { name = "a"; system = "s"; builder = "b"; };
in (derivation { name = "d"; system = "s"; builder = "b"; x = a.drvPath; }).drvPath
