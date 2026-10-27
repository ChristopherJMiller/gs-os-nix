# An inline array of tables cannot be appended to.
builtins.fromTOML "a = [ { } ]\n[[a]]"
