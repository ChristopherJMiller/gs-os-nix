# mutual recursion through a let, and a thunk forced inside a call
let even = n: if n == 0 then true else odd (n - 1); odd = n: if n == 0 then false else even (n - 1); big = 30; in even big && !odd big
