# genList and map past their old ceilings (~1,460 and ~1,995: one reservation
# for the whole result) to 2,500, built a batch at a time (HEAP.md "Long
# lists", step 1); elements either side of a batch's edge (128); the longest
# list one reservation holds (genList 1,488, map 2,046: prims.c ONE_RESERVE),
# one more, and none. The size and the reads every stress mode can afford:
# under mature stress a foldl' over 2,500 elements ran past 900 s, and
# eval-okay-long-genlist-map, at 15,000, folds over every element.
let
  l = builtins.genList (x: x * 3) 2500;
  m = builtins.map (x: x + 1) l;
in [
  (builtins.length l) (builtins.elemAt l 2499) (builtins.length m)
  (builtins.elemAt m 0) (builtins.elemAt m 2499)
  (builtins.elemAt l 127) (builtins.elemAt l 128) (builtins.elemAt m 255) (builtins.elemAt m 256)
  (builtins.elemAt (builtins.genList (x: x) 1488) 1487) (builtins.elemAt (builtins.genList (x: x) 1489) 1488)
  (builtins.elemAt (builtins.map (x: x) (builtins.genList (x: x) 2046)) 2045)
  (builtins.elemAt (builtins.map (x: x) (builtins.genList (x: x) 2047)) 2046)
  (builtins.genList (x: x) 0) (builtins.map (x: x) [ ])
]
