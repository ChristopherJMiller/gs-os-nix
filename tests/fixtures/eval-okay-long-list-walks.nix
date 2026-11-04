# A mature list past WINDOW_MAX (5,119 elements) walked to its end, a function
# called on each element (HEAP.md "Long lists", step 1): foldl', all, filter.
# Not run under mature stress, where a mature collection at every boundary
# puts each walk's 6,000 calls past the scoreboard's 600 s (foldl' over 600 s,
# all and filter 223 s and 238 s, 2026-11-03); eval-okay-long-list-copies
# walks the same list there, stopping early.
let
  r = builtins.genList (j: j) 100;
  l = builtins.concatLists (builtins.genList (i: r) 60);
  churn = builtins.length (builtins.genList (x: x) 1400);   # 61 KB, its elements never forced
  l2 = builtins.seq l (builtins.seq churn l);
in builtins.seq l2 [
  (builtins.foldl' (a: x: a + x) 0 l2)
  (builtins.all (x: x < 100) l2)
  (builtins.length (builtins.filter (x: x < 3) l2))
]
