# readFileType resolves nothing, and Nix refuses a link among the ancestors:
# "path '.../link-to-dir' is a symlink", which tryEval does not catch.
builtins.tryEval (builtins.readFileType ./gsos/link-to-dir/inner)
