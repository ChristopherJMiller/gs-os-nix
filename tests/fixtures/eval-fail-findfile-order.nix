# Every element is forced and coerced before the name: the path's type
# error, which tryEval does not catch, comes first.
builtins.tryEval (builtins.findFile [ { path = 1; } ] (throw "the name"))
