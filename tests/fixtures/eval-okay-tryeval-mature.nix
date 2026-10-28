# A tryEval's catch frame keeps the environment to resume in, and a
# collection of mature space must rewrite it like any other root (HEAP.md
# "Collecting mature space"). Under mature stress the expression it catches
# collects mature space many times over -- so the let's environment moves,
# and its old address is reused -- and then throws; what follows the catch,
# in the same expression, reads the let's variables from the frame's
# environment. Not rewritten, it was the old address.
let
  double = n: s: if n == 0 then s else double (n - 1) (s + s);
  work = i: builtins.stringLength (double 12 (toString i));
  a = "kept";
  b = 7;
in [
  ((builtins.tryEval (builtins.seq (builtins.foldl' (acc: i: acc + work i) 0 (builtins.genList (i: i) 24))
                                   (throw "no"))).success == false && a == "kept" && b == 7)
  (a + toString b)
]
