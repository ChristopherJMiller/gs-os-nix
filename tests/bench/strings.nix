# Strings: toString, concatenation by interpolation and +, joining, slicing.
let
  parts = builtins.genList (i: "${toString (i * 7)}-${toString i}") 800;
  joined = builtins.concatStringsSep "," parts;
in [ (builtins.stringLength joined) (builtins.substring 600 30 joined) (builtins.hashString "sha256" joined) ]
