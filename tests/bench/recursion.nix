# Closures and laziness: a non-tail recursion, map and filter, and a sort.
let
  fib = n: if n < 2 then n else fib (n - 1) + fib (n - 2);
  xs = builtins.genList (i: (i * 7919) - (i / 13) * 101) 300;
in [
  (fib 15)
  (builtins.length (builtins.filter (x: x / 2 * 2 == x) (map (x: x * 3) xs)))
  (builtins.elemAt (builtins.sort (a: b: a < b) xs) 150)
]
