# toXML forces every value: a throwing one is the error.
builtins.toXML { a = 1; b = throw "b"; }
