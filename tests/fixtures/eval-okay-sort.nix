# builtins.sort: Nix 2.34's peeksort (sort.hh) with orderly comparators.
# Each element is one case, answered by the host: the small cases, the
# insertion sort up to 16, natural runs either way, merges of pseudo-random
# lists, stability. eval-okay-sort-noorder has the comparators that are no
# order at all (two files, so each fits the GC-stress cycle budget).
let
  mod = a: m: a - (a / m) * m;
  # residues mod 5 vary (a linear sequence's would not, and `noorder` would be
  # a constant): found with a throwaway model to tell each planted bug apart
  rand = n: seed: builtins.genList (i: mod (i * i * 7919 + i * 104729 + seed * 13) 1009) n;
in [
  (builtins.sort builtins.lessThan [ 3 1 2 ])
  (builtins.sort (a: b: a < b) [ ]) (builtins.sort (throw "f is not forced") [ ])
  (builtins.sort (a: b: a < b) [ 1 ]) (builtins.sort (a: b: a < b) [ 2 1 ])
  (builtins.sort (a: b: true) [ 1 2 3 ]) (builtins.sort (a: b: false) [ 3 1 2 ])
  (builtins.sort builtins.lessThan (rand 16 1)) (builtins.sort builtins.lessThan (rand 17 2))
  (builtins.sort builtins.lessThan (rand 30 7))
  (map (x: x.v) (builtins.sort (a: b: a.k < b.k) (builtins.genList (i: { k = mod (i * 37) 5; v = i; }) 30)))
  (builtins.sort (a: b: a < b) (builtins.genList (i: if i < 15 then i else 30 - i) 30))
  (builtins.sort (a: b: a < b) (builtins.genList (i: if i < 20 then 20 - i else i) 30))
  (builtins.sort (a: b: a < b) (builtins.genList (i: 30 - i) 30))
  (builtins.sort (a: b: a > b) [ "b" "a" "c" ])
  (builtins.sort builtins.lessThan [ [ 1 2 ] [ 1 ] [ 0 5 ] ])
  (builtins.sort (a: b: a < b) [ 1.5 1 2 ])
]
