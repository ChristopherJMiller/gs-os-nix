# fromJSON: raw bytes must be UTF-8: half of a two-byte character.
builtins.fromJSON ("\"" + builtins.substring 0 1 "é" + "\"")
