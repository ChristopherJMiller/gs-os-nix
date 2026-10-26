# ... and so is a set with __toString.
let n = { __toString = s: "a"; }; in { ${n} = 1; }
