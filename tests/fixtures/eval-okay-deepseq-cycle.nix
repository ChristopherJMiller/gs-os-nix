# deepSeq must terminate on a cyclic structure, as Nix's does
let as = { x = 1; y = as; }; l = [ l ]; in [ (builtins.deepSeq as 1) (builtins.deepSeq l 2) as.y.y.x ]
