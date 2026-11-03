# Keeps ~1.2 MB live on the card at once: 280 forced rows of 100 thunks, every
# row live until the fold ends. A 1 MB card's half cannot hold it (E_NOMEM);
# a card whose halves are bigger, and a VM that uses the size the card reports
# rather than a constant, answers (HEAP.md "Long lists", step 0).
let
  rows = builtins.genList (i: builtins.genList (j: i * j) 100) 280;
in builtins.foldl' (acc: row: acc + builtins.length row + builtins.elemAt row 99) 0 rows
