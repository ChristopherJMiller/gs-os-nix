# Computed attribute names (BYTECODE.md "Dynamic attribute names"): each
# element is one rule, answered by the host.
let
  n = "a";
  m = "b";
  z = null;
  s = { a = 1; b = 2; };
  f = k: { ${k} = k; };
  t2 = { x = n + m; };
in [
  { ${n} = 1; }                                   # computed
  { ${n} = 1; b = 2; } { ${m} = 1; a = 2; }       # among static ones, sorted either way
  (builtins.attrNames { ${"z"} = 1; ${m} = 2; y = 3; ${n} = 4; })
  { ${z} = 1; c = 3; } { ${z} = throw "never"; }  # null drops the attribute, value unforced
  { "${n}" = 1; } { "x${n}y" = 1; } { ${n + m} = 1; }
  { ${n}.b = 1; }                                 # a computed prefix
  { a.${m} = 1; a.c = 2; } { a = { ${m} = 1; }; a.c = 2; }   # computed inside a static set: merged
  (rec { a = "x"; ${a} = 1; }) (rec { a = 1; ${m} = a; })    # rec: names and values see the set
  (rec { ${"q"} = 1; r = q; }) (let ${"p"} = 1; in p)         # ${"literal"} is a static name
  { ${"a"}.b = 1; ${"a"}.c = 2; }                 # ... which merges
  (f "a") (f "b")
  s.${n} s.${m} s.${"a"} (s.${"c"} or 9) (s ? ${n}) (s ? ${"c"})
  { x = { ${n} = 1; }; }.x.${n}
  (let t = { a.b = 1; }; in [ t.${n}.b (t ? ${n}.b) (t.${n}.${m}) (t.${m}.c or 3) ])
  ({ ${n} = 1; } == { a = 1; }) ({ a = 0; } // { ${n} = 1; }) (with { ${n} = 5; }; a)
  { inherit n; ${m} = 1; }
  (builtins.length (builtins.attrNames { ${n} = throw "lazy"; }))   # the value is lazy
  ((builtins.tryEval { ${throw "x"} = 1; }).success)                # the name is not
  (let unused = { ${throw "x"} = 1; }; in 1)                        # ...when the set is evaluated
  { ${""} = 1; } { ${"a b"} = 1; } ({ "" = 1; }.${""})
  { ${t2.x} = 1; } { ${(f "q").q} = 2; } (s.${t2.x} or 4)     # a name that is still a thunk
]
