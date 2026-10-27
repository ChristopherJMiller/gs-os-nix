# A NUL, escaped: Nix's forceNoNullByte.
builtins.fromTOML "a = \"x\\u0000y\""
