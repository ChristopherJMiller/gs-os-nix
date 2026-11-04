# A list past WINDOW_MAX (5,119 elements), mature, read through its header and
# an element at a time -- never viewed whole (HEAP.md "Long lists", step 1).
# Its 6,000 slots share r's 100 elements, so a mature collection at every
# boundary (mature stress) copies little. l is built first; churn then fills
# the nursery, so l is promoted; l2 is a thunk updated with the mature list.
let
  r = builtins.genList (j: j) 100;
  l = builtins.concatLists (builtins.genList (i: r) 60);
  churn = builtins.length (builtins.genList (x: x) 1400);   # 61 KB, its elements never forced
  l2 = builtins.seq l (builtins.seq churn l);
in builtins.seq l2 [
  (builtins.length l2) (builtins.head l2) (builtins.elemAt l2 5999)
  (builtins.typeOf l2) (builtins.isList l2) churn
]
