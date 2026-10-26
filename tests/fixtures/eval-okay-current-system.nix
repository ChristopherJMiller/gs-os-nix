# builtins.currentSystem is "w65-gsos" (DECISIONS.md D23): the platform an IIgs
# reports, so a derivation that asks for the current system asks for an IIgs
# builder. Generated with `nix-instantiate --eval --strict --option system
# w65-gsos`, which makes the host report the same.
let d = derivation { name = "here"; system = builtins.currentSystem; builder = "/bin/sh"; };
in [ builtins.currentSystem (builtins ? currentSystem) (with builtins; currentSystem) d.drvPath d.system ]
