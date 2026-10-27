# builtins.hashString with every algorithm Nix has without an experimental
# feature, at each padding edge: a 64-byte block (md5, sha1, sha256) takes
# 55 bytes and the length in one block, 56 needs two; a 128-byte block
# (sha512) 111 and 112. 9000 bytes is a length whose bit count needs three
# bytes. The text holds bytes >= 0x80 (UTF-8), which a signed char breaks.
let
  base = "The quick brown fox jumps over the lazy dog: é ü ß 漢字 ∑ 0123456789. ";
  long = builtins.concatStringsSep "" (builtins.genList (i: base) 130);
  text = n: builtins.substring 0 n long;
  lengths = [ 0 1 3 55 56 57 63 64 65 111 112 113 119 120 127 128 129 239 240 1000 9000 ];
in
  map (algo: map (n: builtins.hashString algo (text n)) lengths) [ "md5" "sha1" "sha256" "sha512" ]
