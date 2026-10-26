# A set's type is forced first, to see whether it is a derivation.
builtins.toXML { a = throw "a"; type = throw "type"; }
