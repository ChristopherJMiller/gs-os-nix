# Fixed-output derivations (docs/DERIVATION.md "Fixed outputs"). The output
# path is made from the declared hash, not from the build, and a derivation
# that depends on one sees only that hash. So `other` below, which differs from
# `flat` only in having a builder from another derivation, has flat's output
# path and its own .drv path; and the derivation `d` built on either of them
# has one output path and two .drv paths. Every spelling of one hash Nix
# accepts gives one output path (the `same` elements); every other element is
# decided by one rule.
let
  fod = a: derivation ({ name = "f"; system = "s"; builder = "b"; } // a);
  sri = "sha256-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=";
  hex = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
  n32 = "0mdqa9w1p6cmli6976v4wi0sw9r4p5prkj7lzfd1877wk11c9c73";
  b64 = "47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=";
  flat = fod { outputHash = sri; };
  same = a: (fod a).outPath == flat.outPath;
  tool = derivation { name = "tool"; system = "s"; builder = "/bin/sh"; };
  other = fod { outputHash = sri; builder = "${tool}/bin/fetch"; };
  dep = f: derivation { name = "d"; system = "s"; builder = "/bin/sh"; src = f; };
  rec' = fod { outputHash = sri; outputHashMode = "recursive"; };
  sha1 = "da39a3ee5e6b4b0d3255bfef95601890afd80709";
  sha512 = "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e";
in [
  flat.outPath flat.drvPath
  (same { outputHash = hex; outputHashAlgo = "sha256"; })
  (same { outputHash = "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855"; outputHashAlgo = "sha256"; })
  (same { outputHash = "sha256:${hex}"; })
  (same { outputHash = "sha256:${n32}"; })
  (same { outputHash = n32; outputHashAlgo = "sha256"; })
  (same { outputHash = b64; outputHashAlgo = "sha256"; })
  (same { outputHash = "sha256:${b64}"; })
  (same { outputHash = "sha256-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU"; })      # no padding
  (same { outputHash = "sha256-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFV="; })     # leftover bits unread
  (same { outputHash = "${sri}!!"; })                                                # nothing after '=' is read
  (same { outputHash = sri; outputHashAlgo = "foo"; })                               # an unknown algorithm is dropped
  (same { outputHash = sri; outputHashAlgo = "sha256"; outputHashMode = "flat"; outputs = [ "out" ]; })
  rec'.outPath rec'.drvPath                                                          # "source:", and "r:sha256"
  ((fod { outputHash = sri; outputHashMode = "nar"; }).outPath == rec'.outPath)
  (fod { outputHash = sha1; outputHashAlgo = "sha1"; }).outPath
  (fod { outputHash = sha1; outputHashAlgo = "sha1"; outputHashMode = "recursive"; }).outPath
  (fod { outputHash = "md5:d41d8cd98f00b204e9800998ecf8427e"; }).drvPath
  (fod { outputHash = sha512; outputHashAlgo = "sha512"; outputHashMode = "recursive"; }).drvPath
  (fod { outputHash = ""; outputHashAlgo = "sha256"; }).outPath                      # all zeros
  (fod { outputHashMode = "nar"; outputHashAlgo = "sha256"; }).drvPath               # no outputHash: not fixed
  (fod { __ignoreNulls = true; outputHash = null; }).drvPath                         # likewise
  (other.outPath == flat.outPath) other.drvPath
  (dep flat).outPath (dep flat).drvPath
  ((dep other).outPath == (dep flat).outPath) (dep other).drvPath
  "${flat}/x"
]
