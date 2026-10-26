# A string that is not UTF-8 -- here the first byte of a two-byte character --
# is nlohmann's type_error 316 when it dumps.
builtins.toJSON (builtins.substring 0 1 "é")
