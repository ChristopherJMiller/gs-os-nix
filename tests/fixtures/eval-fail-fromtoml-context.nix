# A string without context.
builtins.fromTOML "${builtins.toFile "t" "a = 1"}"
