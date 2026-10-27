# A strict fold over a list: a primop calling back into Nix per element.
builtins.foldl' (a: b: a + b * 3) 0 (builtins.genList (x: x) 1000)
