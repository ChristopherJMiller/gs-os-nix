# A computed name must be a string: an integer is a type error.
let n = 1; in { ${n} = 1; }
