# nix32 sha256: 52 digits hold 260 bits, and the top four must be zero ("z" is 31).
(derivation { name = "f"; system = "s"; builder = "b"; outputHash = "sha256:zmdqa9w1p6cmli6976v4wi0sw9r4p5prkj7lzfd1877wk11c9c73"; }).outPath
