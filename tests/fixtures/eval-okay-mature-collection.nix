# Mature space is collected (HEAP.md "Collecting mature space"). Each round
# doubles a string to 16 KB, and what is live when the nursery fills is
# promoted -- so the rounds promote several of the card's 512 KB halves'
# worth in all, while almost nothing stays live. With nothing collecting
# mature space, the half filled and this was E_NOMEM a third of the way in;
# now the card's MARK and COMPACT empty it whenever less than a promotion's
# room is left.
let
  double = n: s: if n == 0 then s else double (n - 1) (s + s);
  round = i: builtins.stringLength (double 14 (toString (i - i)));
in builtins.foldl' (acc: i: acc + round i) 0 (builtins.genList (i: i) 100)
