# Floats (docs/FLOAT.md): IEEE-754 binary64, bit for bit the host's, each rule
# one element. The printer is the harness's (%g, 6 digits); toString is the
# VM's own (%f, exact). `nan` and `inf` are made the only way Nix can.
let
  inf = 1.0e308 * 10;
  nan = inf - inf;
  nz = 0.0 * -1;
in [
  1.5 2.0 .5 1. 1.0e5 1.0E-5 0.0e-400 2.2250738585072014e-308 1.7976931348623157e308
  (1 + 1.5) (1.5 + 1) (7 / 2.0) (7.0 / 2) (2 * 0.1) (0.1 + 0.2) (0.1 + 0.2 == 0.3) (1.0 / 3)
  (- 2.5) (- 0.0) nz (0 - nz) (nz * nz) (1 / inf) (-1 / inf)
  inf (0.0 - inf) nan (nan + 1) (inf * 0) (inf / inf)
  (1 == 1.0) (1.0 == 1) (9007199254740993 == 9007199254740992.0) (2 < 2.5) (2.5 < 2) (1.0 <= 1) (nz == 0.0)
  (nan == nan) ([ nan ] == [ nan ]) (nan < 1) (1 < nan) (nan <= 1) (nan >= 1) (nan > 1) ({ a = nan; } == { a = nan; })
  (builtins.floor 2.7) (builtins.floor (-2.5)) (builtins.ceil 2.0000001) (builtins.ceil (-0.5)) (builtins.floor 5) (builtins.ceil nz)
  (builtins.floor (-9223372036854775807.0 - 1))
  (toString 0.1) (toString 4.2) (toString nz) (toString nan) (toString inf) (toString 1.0e30) (toString 0.0000005) (toString 0.0000015)
  (toString [ 1.5 2 ]) "${toString 2.5}"
  (builtins.typeOf 1.0) (builtins.isFloat 1.0) (builtins.isFloat 1) (builtins.isInt 1.0)
  (builtins.add 1 0.5) (builtins.sub 1 0.5) (builtins.mul 3 0.5) (builtins.div 1 4.0) (builtins.lessThan 1 1.5)
  123456.7 1234567.0 0.00001234565 999999.5 (2.2250738585072014e-308 / 1024) 3.14159265358979 1.0e21
  (let x = 1.5; in x * x) (builtins.foldl' (a: b: a + b) 0 [ 0.1 0.2 0.3 ])
  (1.5 == "a") ([ 1.0 ] < [ 2 ]) ({ a = 1; } == { a = 1.0; }) ([ 1 ] == [ 1.0 ]) (builtins.isFloat (1 + 0.0))
  (9223372036854775807 + 1.0) ((- 9223372036854775807) - 2.0) (builtins.ceil 9007199254740992) (toString (0.1 * 3))
]
