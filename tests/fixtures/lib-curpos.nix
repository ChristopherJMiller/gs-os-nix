# Imported by eval-okay-import-curpos.nix. Not a test itself. Its __curPos is
# at a different offset than any `__curPos` in the importer, so a parser that
# reads the importer's bytes for this file's tokens takes it for a variable.
{ where = __curPos; }
