# builtins.sort with comparators that are no order at all, where Nix 2.34's
# peeksort (sort.hh) shows its exact comparisons: the answer is whatever its
# insertion sort, run detection and merges make of them, so each element
# here tells a port that is not step for step from one that is. And a
# comparator that itself sorts, inside tryEval. Answered by the host.
let
  mod = a: m: a - (a / m) * m;
  # residues mod 5 vary (a linear sequence's would not, and `noorder` would be
  # a constant): found with a throwaway model to tell each planted bug apart
  rand = n: seed: builtins.genList (i: mod (i * i * 7919 + i * 104729 + seed * 13) 1009) n;
  noorder = a: b: mod (a * 7 + b * 3) 5 < 2;
in [
  (builtins.sort (a: b: true) (builtins.genList (i: i) 20))
  (builtins.sort noorder (rand 12 3)) (builtins.sort noorder (rand 20 17))
  (builtins.sort noorder (rand 40 5))
  (builtins.sort (a: b: !(builtins.tryEval (builtins.sort (x: y: throw "inner") [ 1 2 ])).success && a < b) (rand 20 9))
]
