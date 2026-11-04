# A mature list past WINDOW_MAX as the whole answer (HEAP.md "Long lists",
# step 1): forced deeply and printed an element at a time, as a person typing
# it at the REPL would see it. Its 6,000 slots share r's 100 elements.
let
  r = builtins.genList (j: j) 100;
  l = builtins.concatLists (builtins.genList (i: r) 60);
  churn = builtins.length (builtins.genList (x: x) 1400);
in builtins.seq l (builtins.seq churn l)
