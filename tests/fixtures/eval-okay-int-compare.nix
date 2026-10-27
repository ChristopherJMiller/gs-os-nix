# == < > <= >= and + - on two INTs, the cases the VM's fast paths decide from
# the bytes (VM.md "Speed"): values that differ only in their high 32 bits,
# only in sign, at each half's edges -- and a sort, which compares through
# the same order.
let
  hi = 4294967296;                       # 2^32
  big = 9223372036854775807;             # 2^63 - 1
in [
  (hi + 1 == 1) (hi == 0) (hi + 5 == hi + 5) (-1 == 0 - 1) (-hi == 0 - hi)
  (-1 < 0) (0 < -1) (-hi < 1) (1 < -hi) (hi < hi + 1) (hi + 1 < hi) (2 * hi + 1 < hi + 2)
  (-1 > -2) (-hi > -hi - 1) (big > -big) (-big - 1 < -big)
  (hi <= hi) (hi + 1 <= hi) (-hi >= -hi) (-hi - 1 >= -hi)
  (big - 1 + 1) (-big + hi - hi) (hi - 1 + 1) (0 - hi + hi + 7)
  (builtins.sort (a: b: a < b) [ hi 3 (-hi) (-1) 0 (hi + 1) (-hi - 1) big (-big - 1) 1 ])
]
