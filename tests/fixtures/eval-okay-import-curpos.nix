# __curPos in an imported file is that file's position: the parser reads the
# imported file's own bytes, not the importer's, to tell `__curPos` from an
# identifier (the on-target parser once read SRC_BASE's).
let lib = import ./lib-curpos.nix; in [ lib.where.line lib.where.column __curPos.line ]
