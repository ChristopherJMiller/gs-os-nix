# The write barrier's remembered set (docs/DECISIONS.md D20). The spines of xs
# and fs are built first; `churn` then fills the nursery, so a collection
# promotes their ten unforced element thunks each to the card. Forcing one
# stores its value into a MATURE thunk -- and a list (the thunk becomes an
# INDIRECT to it) or a closure (copied in place, its environment young) is a
# young pointer on the card, reachable only through the remembered set until
# the next collection promotes it and writes the card address back. `churn2`
# forces that collection; `second` then reads every element again through the
# rewritten fields.
#
# The churn is a few 1.8 KB strings rather than many small objects, so the
# nursery fills in few instructions -- which keeps this cheap under GC stress.
# (GC stress at every boundary does not exercise the set at all: every value is
# already mature when it is stored. The set is tested by the normal run, by
# A2MIN_RSET_LIMIT=1, which makes it fill, and by stress at every 2nd or 3rd
# boundary, A2MIN_GC_PERIOD, which leaves some stores young.)
let
  xs = builtins.genList (i: [ i (i * 2) ]) 10;
  fs = builtins.genList (i: (y: y + i)) 10;
  a = "0123456789abcdef";
  s = let b = a + a + a + a; c = b + b + b + b; in c + c + c + c + c + c + c;   # 1792 bytes
  burn = k: builtins.deepSeq (builtins.genList (i: s + builtins.toString i) k) k;
  churn = burn 19 + burn 19;     # never more than ~34 KB live: the fixture also runs with no card
  churn2 = burn 19 + burn 19;
  sumPairs = l: builtins.foldl' (acc: p: acc + builtins.elemAt p 0 + builtins.elemAt p 1) 0 l;
  sumCalls = l: builtins.foldl' (acc: f: acc + f 1) 0 l;
  first = [ (sumPairs xs) (sumCalls fs) ];
  second = [ (sumPairs xs) (sumCalls fs) (builtins.elemAt xs 9) ((builtins.elemAt fs 7) 100) ];
in builtins.seq (builtins.length xs + builtins.length fs)
     (builtins.seq churn (builtins.deepSeq first (builtins.seq churn2 (first ++ second))))
