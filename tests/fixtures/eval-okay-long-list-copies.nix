# A list past WINDOW_MAX (5,119 elements), mature, copied and walked an
# element at a time (HEAP.md "Long lists", step 1): tail, ++, concatLists of
# it, elem, any, all over it, toJSON of it. The walks stop early, so this runs
# in every mode; eval-okay-long-list-walks walks it to the end. The lists share
# r's 100 elements, so mature stress copies little.
let
  r = builtins.genList (j: j) 100;
  l = builtins.concatLists (builtins.genList (i: r) 60);
  churn = builtins.length (builtins.genList (x: x) 1400);   # 61 KB, its elements never forced
  l2 = builtins.seq l (builtins.seq churn l);
in builtins.seq l2 [
  (builtins.length (builtins.tail l2)) (builtins.elemAt (builtins.tail l2) 5998)
  (builtins.length (l2 ++ [ 1 ])) (builtins.elemAt ([ 7 ] ++ l2) 6000)
  (builtins.length (builtins.concatLists [ l2 l2 ]))
  (builtins.elem 99 l2) (builtins.any (x: x == 99) l2) (builtins.all (x: x < 50) l2)
  (builtins.stringLength (builtins.toJSON l2))
]
