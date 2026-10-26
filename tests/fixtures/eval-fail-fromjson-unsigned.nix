# fromJSON: an unsigned number past INT64_MAX is outside the Nix integer range.
builtins.fromJSON "9223372036854775808"
