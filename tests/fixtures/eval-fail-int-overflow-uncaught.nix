# An overflow is an evaluation error, which tryEval does not catch: only
# throw and assert are.
(builtins.tryEval (9223372036854775807 + 1)).success
