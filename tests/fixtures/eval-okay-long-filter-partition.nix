# filter, partition, concatMap and catAttrs past their old ceilings (~1,020
# kept, partition ~500, at the top level: results staged on the value stack),
# into lists, to 15,000 (HEAP.md "Long lists", step 1); at 1,100,
# eval-okay-long-filter-partition-small, eval-okay-long-concatmap-small and
# eval-okay-long-catattrs-small, which every stress mode can afford. A throw
# inside filter's predicate, caught, leaves a half-filled
# buffer behind; the lists after it must not mind. Each keeps the stack's
# path where its staging fits with room for its calls (prims.c STAGE_FITS):
# filter 700 and partition 300 there, 900 and 500 past it.
let
  l = builtins.genList (x: x) 15000;
  f = builtins.filter (x: x / 2 * 2 == x) l;
  p = builtins.partition (x: x < 9000) l;
  c = builtins.concatMap (x: [ x x ]) (builtins.genList (x: x) 7000);
  a = builtins.catAttrs "v" (builtins.genList (x: { v = x; }) 12000);
  t = builtins.tryEval (builtins.filter (x: if x == 3000 then throw "half" else true) l);
  even = x: x / 2 * 2 == x;
  g = builtins.genList (x: x);
in [
  (builtins.length f) (builtins.elemAt f 7499) (builtins.length p.right)
  (builtins.length p.wrong) (builtins.elemAt p.wrong 0) (builtins.length c)
  (builtins.elemAt c 13999) (builtins.length a) (builtins.elemAt a 11999)
  t.success (builtins.length (builtins.filter (x: true) l))
  (builtins.length (builtins.filter even (g 700))) (builtins.length (builtins.filter even (g 900)))
  (builtins.length (builtins.partition even (g 300)).wrong) (builtins.length (builtins.partition even (g 500)).wrong)
]
