# eval-okay-long-filter-partition's concatMap at 1,100, the size every stress
# mode can afford (eval-okay-long-filter-partition-small has why): past what
# the value stack staged (HEAD's ceiling at the top level: ~1,020), into a
# buffer list (HEAP.md "Long lists", step 1). The mapped lists are young when
# they go into a buffer a collection may have promoted, so they go in through
# the barrier: without it, a slot read later is whatever list was made where
# its list was, which elements in the middle show. With catAttrs in the same
# test, mature stress took 314 s of the 600.
let c = builtins.concatMap (x: [ x x ]) (builtins.genList (x: x) 1100); in
[ (builtins.length c) (builtins.elemAt c 0) (builtins.elemAt c 1001) (builtins.elemAt c 1500)
  (builtins.elemAt c 2000) (builtins.elemAt c 2199) ]
