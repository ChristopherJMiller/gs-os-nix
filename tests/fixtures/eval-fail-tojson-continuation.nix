# ...and so is a continuation byte with no lead byte.
builtins.toJSON (builtins.substring 1 1 "é")
