# A dotted key cannot reopen a table a header defined.
builtins.fromTOML "[a.b]\nx = 1\n[a]\nb.y = 2"
