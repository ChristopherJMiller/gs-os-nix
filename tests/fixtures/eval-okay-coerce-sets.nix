# String coercion of attribute sets: Nix's coerceToString (prims.c coerce_top).
# __toString is called with the set itself and wins over outPath; whatever it
# returns is coerced again by the same rules; "${drv}" is an outPath coercion,
# which is how a derivation becomes its store path inside a string (M4).
let
  drv = { type = "derivation"; outPath = "/nix/store/abc-hello"; name = "hello"; };
  both = { __toString = self: "via-toString"; outPath = "via-outPath"; };
  counter = { n = 41; __toString = self: toString (self.n + 1); };
  nested = { __toString = _: { outPath = { __toString = _: "deep"; }; }; };
  truthy = { __toString = _: true; };
  thrower = { __toString = _: throw "boom"; };
in [
  "${drv}/bin/hello"
  (drv + "")
  ("prefix-" + drv)
  "${both}"
  (toString both)
  "${counter}"
  (toString nested)
  (toString truthy)
  (builtins.stringLength drv)
  (builtins.substring 0 4 both)
  (builtins.concatStringsSep "," [ drv both "plain" ])
  (builtins.tryEval "${thrower}").success
  (toString [ drv both 1 true null ])
]
