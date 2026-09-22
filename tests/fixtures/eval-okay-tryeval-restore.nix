# after a caught throw the VM must be fully restored: stack, env, and a later throw must still be fatal-ly correct
let a = builtins.tryEval (throw "e"); b = let x = 10; in x + (if a.success then 1 else 2); in [ a.success b (builtins.tryEval (b + 1)).value ]
