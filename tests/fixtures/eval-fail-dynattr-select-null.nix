# In a selection a null key is a type error, not a miss.
let n = null; in { a = 1; }.${n}
