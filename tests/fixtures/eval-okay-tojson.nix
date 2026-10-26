# builtins.toJSON: Nix's value-to-json.cc and nlohmann/json's dump, each
# element one rule, answered by the host.
let
  d = derivation { name = "d"; system = "s"; builder = "/b"; };
  inf = 1.0e308 * 10;
in [
  (builtins.toJSON null) (builtins.toJSON true) (builtins.toJSON false)
  (builtins.toJSON 0) (builtins.toJSON (-17)) (builtins.toJSON 9223372036854775807) (builtins.toJSON (-9223372036854775807 - 1))
  (builtins.toJSON 1.0) (builtins.toJSON 0.1) (builtins.toJSON 1.0e15) (builtins.toJSON 1.0e-5) (builtins.toJSON 0.0001)
  (builtins.toJSON (0.1 + 0.2)) (builtins.toJSON (0.0 * -1)) (builtins.toJSON inf) (builtins.toJSON (inf - inf))
  (builtins.toJSON "") (builtins.toJSON "plain") (builtins.toJSON "tab\there") (builtins.toJSON "line\nbreak\r")
  (builtins.toJSON "quote \" backslash \\ slash /") (builtins.toJSON "é ü 日本 😀") (builtins.toJSON "a\x7fb")
  (builtins.toJSON [ ]) (builtins.toJSON { }) (builtins.toJSON [ 1 [ 2 [ 3 ] ] { } ])
  (builtins.toJSON { b = 1; a = [ null true 1.5 "x" ]; "c d" = { }; "" = 0; })
  (builtins.toJSON { "a\"b" = 1; "\t" = 2; })
  (builtins.toJSON { outPath = 5; }) (builtins.toJSON { outPath = { x = [ 1 ]; }; })
  (builtins.toJSON { __toString = s: "t"; outPath = 5; }) (builtins.toJSON { __toString = s: /x/y; })
  (builtins.toJSON { __toString = s: s.v; v = "inner"; }) (builtins.toJSON [ { __toString = s: "1"; } { __toString = s: "2"; } ])
  (builtins.toJSON d) (builtins.hasContext (builtins.toJSON d)) (builtins.hasContext (builtins.toJSON [ 1 "x" ]))
  (builtins.attrNames (builtins.getContext (builtins.toJSON { a = d; b = "${d}"; c = d.drvPath; })))
  (builtins.toJSON (let x = 1 + 2; in { inherit x; y = [ x x ]; }))
  (builtins.toJSON (builtins.genList (i: i * i) 12))
]
