# ...and a __functor set is not one, for functionArgs.
builtins.functionArgs { __functor = s: { a }: a; }
