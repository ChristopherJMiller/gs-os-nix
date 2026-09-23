# import is resolved by the generator (docs/BYTECODE.md "Imports"): the file is
# compiled into its own chunk and reached through a CLOSED_THUNK, so it is
# evaluated in its own scope and only once, however many times it is forced.
let
  lib = import ./lib-imported.nix;
  again = import ./lib-imported.nix;
in [ (lib.bump lib.answer) lib.nested.deep (again.answer == lib.answer) ]
