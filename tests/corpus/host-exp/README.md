# The host's answers where tvix's disagree

The vendored tvix corpus (`../tvix/`) was written against tvix, and in a few
places its `.exp` is not what C++ Nix 2.34 answers. The host is the truth here
(DECISIONS.md D7), so `scripts/scoreboard.py` scores such a test against the
file of the same path in this directory, and the corpus itself stays as
vendored. Each file is `nix-instantiate --eval --strict` of the test, on the
host (Nix 2.34.9); the scoreboard lists each one it uses, and refuses one that
agrees with tvix's `.exp` after all.

| test | tvix's `.exp` | Nix 2.34 | why |
|---|---|---|---|
| `tvix_tests/eval-okay-builtins-replace-strings-propagate-catchable` | 3rd element `false` | `true` | `replaceStrings [ "a" "b" ] [ "c" (throw "d") ] "a"`: Nix forces a `to` only when its `from` matches (`primops.cc` prim_replaceStrings), and `"b"` never does; tvix forces them all |
