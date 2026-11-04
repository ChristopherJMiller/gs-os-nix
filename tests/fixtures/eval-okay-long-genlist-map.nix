# genList and map past their old ceilings (~1,460 and ~1,995: one reservation
# for the whole result) to 15,000, built a batch at a time (HEAP.md "Long
# lists", step 1); and the longest list one reservation holds (genList
# 1,488, map 2,046: prims.c ONE_RESERVE), one more, and none.
let
  l = builtins.genList (x: x * 3) 15000;
  m = builtins.map (x: x + 1) l;
in [
  (builtins.length l) (builtins.elemAt l 14999) (builtins.length m)
  (builtins.elemAt m 0) (builtins.elemAt m 14999) (builtins.foldl' (a: x: a + x) 0 m)
  (builtins.elemAt (builtins.genList (x: x) 1488) 1487) (builtins.elemAt (builtins.genList (x: x) 1489) 1488)
  (builtins.elemAt (builtins.map (x: x) (builtins.genList (x: x) 2046)) 2045)
  (builtins.elemAt (builtins.map (x: x) (builtins.genList (x: x) 2047)) 2046)
  (builtins.genList (x: x) 0) (builtins.map (x: x) [ ])
]
