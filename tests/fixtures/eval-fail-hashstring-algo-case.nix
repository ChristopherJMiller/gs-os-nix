# The algorithm's name is matched exactly: "SHA1" is not sha1 (Nix: unknown
# hash algorithm), an error tryEval does not catch.
builtins.tryEval (builtins.hashString "SHA1" "x")
