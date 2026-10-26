# s is forced before any to: the throwing s, not the throwing to, is the error.
builtins.replaceStrings [ "a" ] [ (throw "to") ] (throw "s")
