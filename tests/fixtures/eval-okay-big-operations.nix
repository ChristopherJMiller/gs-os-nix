# One operation that allocates more than the nursery has free must collect
# first (vm.h nursery_reserve). Collections used to run only at bytecode
# boundaries with a 2 KB margin, so `genList 300` -- 13 KB in one instruction --
# was E_NOMEM or not depending on where the nursery happened to be. Each
# operation below allocates several KB at once and runs six times, so some
# of those runs must start with less free space than they need.
let
  n = 300;
  xs = builtins.genList (i: i) n;
  sum = l: builtins.foldl' (a: b: a + b) 0 l;
  rounds = f: builtins.genList f 6;
  set = builtins.listToAttrs (builtins.genList (i: { name = "k${toString i}"; value = i; }) 200);
  str = builtins.concatStringsSep "," (builtins.genList (i: toString i) 1000);
# Each round reads CONTENT from the young operand, not just a length: a reserve
# that collects under an operation still holding a view of a young operand
# leaves it reading a forwarded object -- the right length, the wrong elements.
in [
  (sum (rounds (j: let l = builtins.genList (i: i + j) n; in builtins.length l + builtins.elemAt l 299)))
  (sum (rounds (j: let l = builtins.map (x: x + j) xs; in builtins.length l + builtins.elemAt l 299)))
  (sum (rounds (j: let l = xs ++ xs ++ [ j ]; in builtins.length l + builtins.elemAt l 600)))
  (sum (rounds (j: let s = set // { extra = j; }; in builtins.length (builtins.attrNames s) + s.extra + s.k199)))
  (rounds (j: builtins.substring 3880 20 (str + toString j)))
]
