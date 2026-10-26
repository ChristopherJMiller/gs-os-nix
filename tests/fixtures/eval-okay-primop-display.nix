# How the printer shows a function (BYTECODE.md "Deep force and the printer"):
# a lambda is <LAMBDA>, a primop <PRIMOP>, and a primop given some but not all
# of its arguments <PRIMOP-APP>. Each element is one case, answered by the
# host: arities 1, 2 and 3 at every stage, a global and a builtins. name for
# the same primop, a lambda that only calls one (still a lambda), and
# `derivation`, which in Nix is a lambda around a primop.
[
  builtins.isFloat builtins.add (builtins.add 1) builtins.foldl' (builtins.foldl' (a: b: a))
  (builtins.foldl' (a: b: a) 0) map (map (x: x)) toString builtins.toString throw
  (x: x) (a: b: builtins.add a b) ((a: b: builtins.add a b) 1) derivation
  builtins.substring (builtins.substring 0) (builtins.substring 0 1)
  (let f = builtins.add; in f) { f = builtins.length; }
]
