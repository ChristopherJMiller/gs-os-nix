# hashString hashes a string's bytes and drops its context, whichever
# algorithm: the digest of a derivation's output path, with no context.
let
  d = derivation { name = "d"; system = "s"; builder = "b"; };
  s = "${d}/bin";
in
  map (algo: let h = builtins.hashString algo s; in [ h (builtins.getContext h) ]) [ "md5" "sha1" "sha512" ]
