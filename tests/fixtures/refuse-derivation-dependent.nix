# Valid Nix that the VM must REFUSE, not answer: until string context exists,
# `b` would get a's output path as plain text, with no inputDrvs, and a
# well-formed, wrong .drv path (the host's is f52xizra...-b.drv). So a second
# derivationStrict in one program is E_UNSUP (docs/DERIVATION.md "Refused").
let
  a = derivation { name = "a"; system = "s"; builder = "b"; };
  b = derivation { name = "b"; system = "s"; builder = "${a}/bin/sh"; };
in b.drvPath
