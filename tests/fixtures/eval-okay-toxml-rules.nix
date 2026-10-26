# builtins.toXML: Nix's value-to-xml.cc through its XMLWriter, each element
# one rule, answered by the host.
let
  d = derivation { name = "d"; system = "s"; builder = "/b"; };
  x = builtins.toXML;
in [
  (x null) (x true) (x 17) (x (-3)) (x "plain") (x "esc \" < > & ' \n end") (x /some/path)
  (x 1.5) (x 0.1) (x 1234567.0) (x 0.0001) (x 0.00001) (x (0.0 * -1)) (x (1.0e308 * 10))
  (x [ ]) (x { }) (x [ 1 [ 2 ] { a = 3; } ])
  (x { b = 1; a = "s"; "&-{" = ";&\""; })
  (x (y: y)) (x ({ a, b ? 1 }: a)) (x ({ a, ... }: a)) (x (s@{ c, ... }: s)) (x ({ }: 1))
  (x builtins.add) (x (builtins.add 1)) (x derivation)
  (x d) (x [ d d ]) (x { p = d; q = d; })
  (x { type = "derivation"; name = "fake"; }) (x { type = "derivation"; drvPath = ""; })
  (x [ d (d // { boom = throw "a repeated derivation is not looked into"; }) ])
  (x { type = "derivation"; hidden = throw "nor is one without a drvPath"; })
  (builtins.hasContext (x d)) (builtins.hasContext (x [ d d ])) (builtins.hasContext (x { type = "derivation"; drvPath = "${d}"; }))
]
