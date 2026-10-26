# genericClosure compares keys with Nix's ordering: an int and a string are
# not comparable.
builtins.genericClosure { startSet = [ { key = 1; } { key = "a"; } ]; operator = x: [ ]; }
