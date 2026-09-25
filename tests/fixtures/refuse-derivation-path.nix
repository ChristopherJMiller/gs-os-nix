# A path in a derivation is copied into the store, which needs the store (L7):
# refused, not turned into its text as toString would.
(derivation { name = "p"; system = "s"; builder = ./builder.sh; }).drvPath
