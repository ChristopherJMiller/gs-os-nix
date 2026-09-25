# "duplicate derivation output 'out'"
(derivation { name = "x"; system = "s"; builder = "b"; outputs = [ "out" "out" ]; }).drvPath
