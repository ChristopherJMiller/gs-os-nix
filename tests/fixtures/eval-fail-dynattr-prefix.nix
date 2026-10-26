# A computed prefix does not merge with a static one: two sets named a.
let n = "a"; in { a.b = 1; ${n}.c = 2; }
