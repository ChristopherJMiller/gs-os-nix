# A let binding whose value is a bare name of the same let -- itself, or one
# after it -- is a thunk (compile.c binding(), as nbc): the slot is null until
# the let fills it, and a copy would copy the null. x is never forced: it is
# infinite recursion, in Nix too. front-diff's self-test plants "a binding
# that names itself is a pointer copy"; until 2026-11-04 only a fuzzed mutant
# of another file caught it, and four new fixtures changed the mutants seed 1
# draws.
let
  x = x;
  a = b;
  b = 1;
in [ a b ]
