# readFile refuses a NUL: "cannot be represented as a Nix string".
builtins.readFile ./gsos/binary
