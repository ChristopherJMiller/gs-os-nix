# A header cannot reach into an inline table either.
builtins.fromTOML "a = { x = 1 }\n[a.b]"
