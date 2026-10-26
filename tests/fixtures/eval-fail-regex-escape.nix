# Built strict (as Nix is), POSIX escapes only a special character: \d is an error.
builtins.match "\\d" "1"
