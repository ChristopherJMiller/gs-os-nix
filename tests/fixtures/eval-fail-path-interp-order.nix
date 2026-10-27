# The segments are forced and coerced in order: the integer's type
# error, which tryEval does not catch, before the throw.
builtins.tryEval /${1}${throw "the second"}
