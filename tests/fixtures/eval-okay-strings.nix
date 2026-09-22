let s = "a" + "b${"c"}"; in [ s (builtins.stringLength s) (s == "abc") ("x" < "y" || true) ]
