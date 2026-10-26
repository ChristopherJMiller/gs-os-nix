# Arithmetic at the edges of the 64-bit range that does NOT overflow: each
# element would be E_OVERFLOW with a check one off in either direction. The
# products cover both of the multiply's paths -- two 32-bit operands, decided
# at once, and the exact check -- and a negative product reaching the minimum,
# which a positive one may not.
let max = 9223372036854775807; min = -9223372036854775807 - 1; in [
  (max + 0) (min + 0) (max + min) (min - min) (max - max) (-max) (0 - max) (min + 1 - 1) (max - 1 + 1)
  (max * 1) (min * 1) (max * -1) (1 * min) (4611686018427387904 * -2) (-4611686018427387904 * 2)
  (3037000499 * 3037000499) (-3037000499 * 3037000499) (2147483647 * -2147483648) (2147483648 * 2147483648)
  (-2147483648 * -2147483648) (0 * min) (min * 0)
  (min / 1) (min / 2) (max / -1) (min / max) (max / min)
  (builtins.add max min) (builtins.mul 3 7) (builtins.sub min (-1)) (builtins.div min (-2))
]
