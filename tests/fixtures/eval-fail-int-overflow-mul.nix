# "integer overflow in multiplying 4611686018427387904 * 2": Nix 2.34 checks
# + - * / and errors rather than wrap. 2^62 is wider than 32 bits, so this is
# the exact check, not the fast path; 2^62 * -2 is the minimum and fits
# (eval-okay-int-limits).
let x = 4611686018427387904; in x * 2
