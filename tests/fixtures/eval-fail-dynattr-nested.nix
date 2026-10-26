# Inside a static set merged with its siblings, a computed name that
# equals a static one is still the error.
let n = "b"; in { a = { b = 1; }; a.${n} = 2; }
