# A prefix is a string without context.
builtins.findFile [ { prefix = "${builtins.toFile "p" "x"}"; path = "/p"; } ] "y"
