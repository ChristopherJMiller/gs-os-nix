# An inline table is immutable.
builtins.fromTOML "a = { x = 1 }\na.y = 2"
