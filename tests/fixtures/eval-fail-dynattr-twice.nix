# Two computed names that come out equal are an error.
let n = "a"; in { ${n} = 1; ${n} = 2; }
