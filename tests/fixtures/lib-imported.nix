# Imported by eval-okay-import.nix. Not a test itself: the scoreboard only
# collects eval-okay-*.nix that have a matching .exp.
{
  answer = 41;
  bump = a: a + 1;
  nested = import ./lib-imported-2.nix;
}
