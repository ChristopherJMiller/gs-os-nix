# eval-okay-long-filter-partition's filter and partition at 1,100, the size
# every stress mode and mutation-vm's 500M-cycle budget can afford, past what
# the value stack staged (HEAD's ceilings at the top level: ~1,020 kept,
# partition ~500), into lists (HEAP.md "Long lists", step 1). A throw inside
# filter's predicate, caught, leaves a half-filled buffer behind; the lists
# after it must not mind. Under mature stress (everything live copied at every
# boundary) the four families in one test took 470 s of the 600, so concatMap
# and catAttrs have tests of their own (eval-okay-long-concatmap-small,
# eval-okay-long-catattrs-small), each family here a let of its own, and
# filter's first predicate is a cheap one.
let
  l = builtins.genList (x: x) 1100;
in [
  (let f = builtins.filter (x: x < 550) l; in [ (builtins.length f) (builtins.elemAt f 549) ])
  (builtins.tryEval (builtins.filter (x: if x == 100 then throw "half" else true) l)).success
  (builtins.length (builtins.filter (x: true) l))
  (let p = builtins.partition (x: x < 700) l; in
    [ (builtins.length p.right) (builtins.length p.wrong) (builtins.elemAt p.wrong 0) ])
]
