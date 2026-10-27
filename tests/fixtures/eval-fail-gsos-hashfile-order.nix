# The algorithm is forced, and must be a string, before the path.
builtins.tryEval (builtins.hashFile 1 (throw "the path"))
