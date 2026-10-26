# floor of a NaN has no INT (docs/FLOAT.md).
let inf = 1.0e308 * 10; in builtins.floor (inf - inf)
