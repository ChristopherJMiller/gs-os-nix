# An INT given to floor goes through a double; one that does not come back
# unchanged is an error (Nix issue 12899, docs/FLOAT.md). 2^53 + 1 does not.
builtins.floor 9007199254740993
