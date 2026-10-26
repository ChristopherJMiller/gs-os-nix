# fromJSON: a NUL cannot be in a Nix string (forceNoNullByte).
builtins.fromJSON ''"a\u0000b"''
