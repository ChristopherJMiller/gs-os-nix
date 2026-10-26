# builtins.functionArgs (BYTECODE.md LAMBDA_ARGS): a lambda's formals,
# name -> has a default, though the generator desugars the formals away. Each
# element one case, answered by the host.
let
  f = { z, y ? z }: y;
  g = s@{ b ? 2, a, ... }: a;
  curried = x: { a }: a;
  inList = [ ({ q }: q) ];
  passed = h: builtins.functionArgs h;
in [
  (builtins.functionArgs ({ a, b ? 1, ... }: a)) (builtins.functionArgs ({ }: 1))
  (builtins.functionArgs ({ ... }: 1)) (builtins.functionArgs ({ a }@s: a))
  (builtins.functionArgs f) (builtins.functionArgs g)
  (builtins.functionArgs (x: x)) (builtins.functionArgs builtins.add) (builtins.functionArgs (builtins.add 1))
  (builtins.functionArgs builtins.functionArgs)
  (builtins.functionArgs curried) (builtins.functionArgs (curried 1))
  (builtins.functionArgs (builtins.head inList)) (passed f) (passed (x: { r ? 1 }: r))
  (builtins.functionArgs ({ a ? throw "defaults are not evaluated" }: a))
  (builtins.functionArgs derivation)
  (builtins.functionArgs f == builtins.functionArgs f) (f { z = 5; }) (g { a = 7; })
  (builtins.functionArgs (if true then f else g))
  (map builtins.functionArgs [ f (x: x) g ])
]
