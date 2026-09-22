let f = { __functor = self: x: self.k + x; k = 40; }; in [ (f 2) (builtins.isFunction f) ]
