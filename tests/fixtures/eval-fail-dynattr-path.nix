# ... and so is a path: forceStringNoCtx does not coerce.
let n = ./x; in { ${n} = 1; }
