# The pattern is forceStringNoCtx: one with context is an error.
builtins.match "${builtins.toFile "regex-pattern" "a"}" "a"
