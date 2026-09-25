# A fixed-output derivation: valid Nix, a different path computation, not yet
# implemented -- refused rather than hashed as input-addressed.
(derivation {
  name = "f";
  system = "s";
  builder = "b";
  outputHash = "sha256-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=";
  outputHashMode = "flat";
}).outPath
