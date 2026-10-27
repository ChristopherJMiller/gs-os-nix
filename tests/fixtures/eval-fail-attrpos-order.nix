# The name is forced first: its type error, which tryEval does not catch,
# before the set's throw.
builtins.tryEval (builtins.unsafeGetAttrPos 1 (throw "the set"))
