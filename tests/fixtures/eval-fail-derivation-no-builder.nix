# "required attribute 'builder' missing" -- an empty builder counts as missing.
(derivation { name = "x"; system = "s"; builder = ""; }).drvPath
