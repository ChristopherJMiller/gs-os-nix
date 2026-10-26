# A non-empty startSet needs an operator.
builtins.genericClosure { startSet = [ { key = 1; } ]; }
