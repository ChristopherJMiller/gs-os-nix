# The operator must give a list.
builtins.genericClosure { startSet = [ { key = 1; } ]; operator = x: { key = 2; }; }
