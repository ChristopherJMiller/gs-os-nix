# fromJSON: a BOM is skipped only whole: its first byte alone, then [1].
builtins.fromJSON (builtins.substring 0 1 "﻿" + "[1]")
