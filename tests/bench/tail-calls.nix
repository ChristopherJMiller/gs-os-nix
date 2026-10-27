# The interpreter's core loop, as eval-okay-tail-call-1 has it: a call, an
# environment, a lazy argument, ==, if and +, per iteration.
let f = n: if n == 5000 then n else f (n + 1); in f 0
