# builtins.hashString "sha256" (prims.c): the card's HASH_PUSH for a young string,
# HASH_STREAM for a mature one (hashed where it lives, under GC stress), and
# sha256.c when the verb is masked or there is no card -- checks.verb-economics
# runs all three and requires the same answers. The lengths are FIPS 180-4's
# padding edges: 55 bytes fits one block, 56 needs two, 64 is exactly one; 200
# is four blocks, kept small because checks.verb-economics runs the no-card row
# under GC stress, where a long string's thunks are copied at every boundary.
let
  h = builtins.hashString "sha256";
  rep = n: s: builtins.concatStringsSep "" (builtins.genList (_: s) n);
in [
  (h "")
  (h "abc")
  (h (rep 55 "a"))
  (h (rep 56 "a"))
  (h (rep 64 "a"))
  (h (rep 200 "x"))
  (h "Nix on an Apple IIgs")
]
