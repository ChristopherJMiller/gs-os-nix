# The pattern is compiled before the string is forced, and its error is not
# one tryEval catches -- the throw would be.
builtins.tryEval (builtins.match "a{2,1}" (throw "the string"))
