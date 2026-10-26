# After a range, a - that does not end the bracket is an error in POSIX.
builtins.match "[a-z-9]" "a"
