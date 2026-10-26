# The clash is an evaluation error, which tryEval does not catch.
let n = "a"; in builtins.tryEval { ${n} = 1; a = 2; }
