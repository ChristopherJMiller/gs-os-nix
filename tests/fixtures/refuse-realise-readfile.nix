# A string with a context names store paths Nix would build before reading:
# that needs the store, so it is refused by name (GSOS.md).
let d = derivation { name = "d"; system = "s"; builder = "b"; }; in builtins.readFile "${d}"
