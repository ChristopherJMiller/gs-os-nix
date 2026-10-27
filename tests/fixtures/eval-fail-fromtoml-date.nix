# A date: Nix without parse-toml-timestamps refuses it.
builtins.fromTOML "d = 1979-05-27"
