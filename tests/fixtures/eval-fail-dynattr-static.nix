# A computed name equal to a static one is an error ("dynamic attribute
# already defined"): computed names are never merged.
let n = "a"; in { ${n} = 1; a = 2; }
