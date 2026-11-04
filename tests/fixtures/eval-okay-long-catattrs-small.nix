# eval-okay-long-filter-partition's catAttrs at 1,100, the size every stress
# mode can afford (eval-okay-long-filter-partition-small has why): past what
# the value stack staged (HEAD's ceiling at the top level: ~1,020), into a
# buffer list (HEAP.md "Long lists", step 1). Every set has the name, so the
# buffer is the answer.
let a = builtins.catAttrs "v" (builtins.genList (x: { v = x; }) 1100); in
[ (builtins.length a) (builtins.elemAt a 0) (builtins.elemAt a 1099) ]
