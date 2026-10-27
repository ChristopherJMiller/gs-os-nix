# builtins.fromTOML: toml11 4.4.0's TOML 1.0, as Nix 2.34 parses it
# (docs/TOML.md). Each answer is one rule.
let t = builtins.fromTOML; in [
  # tables
  (t "a.b = 1\na.c = 2")                         # dotted keys extend the table they made
  (t "[a.b]\nx = 1\n[a]\ny = 2")                 # [a], implicit until here, may be defined once
  (t "[[p]]\nn = 1\n[[p]]\nn = 2\n[p.q]\nm = 3")   # [p.q] goes into the last [[p]]
  (t "x = { y.z = 1, w = [ 1, { v = 2 } ] }")
  (t "[a]\nb.c = 1\n[a.b.d]\ne = 2")             # toml11 lets a header extend a dotted table
  (t "\"\" = 1\n'q' = 2\n\"a.b\" = 3\n1234 = 4\nbare-key_x = 5")
  (t "z = 1\nA = 2\na = 3\n\"é\" = 4")           # a set's names in byte order
  (t "")
  (t (builtins.fromJSON "\"\\ufeff\"" + "a = 1"))   # a BOM
  (t "a = 1\r\n[b]\r\nc = 2\r\n")                # CRLF
  (t "# c\n  a = 1 # c\n[b] # c\n# c\nc = [ # c\n 1, # c\n 2 # c\n] # c")
  # strings
  (t "s = \"tab\\tquote\\\"slash\\\\ \\u00e9 \\U0001F600\"")
  (t "s = 'C:\\path\\no escapes'")
  (t "s = \"\"\"\nfirst newline trimmed\n  kept\"\"\"")
  (t "s = \"\"\"one \\\n    two \\\n\n three\"\"\"")       # a line-ending backslash eats the space after
  (t "s = \"\"\"a\"\"\"\"\nt = '''b''''' ")              # quotes against the closing ones
  (t "s = '''\n raw\\n '''")
  # numbers
  (t "a = [ 0x1F, 0o17, 0b101, 1_000, -0, +7, 9223372036854775807, -9223372036854775808, 0x7FFFFFFFFFFFFFFF ]")
  (t "a = [ 1.5, -0.0, 1e5, 6.02E+23, 1_0.2_5, 0e0, 5e-324, 1.7976931348623157e308 ]")
  (map builtins.toString (t "a = [ inf, -inf, nan, -nan, +inf ]").a)
  (t "a = [ true, false, \"mixed\", [ ], { } ]")
  (builtins.typeOf (t "a = 1.0").a)
  # 32 arrays and inline tables in each other, the VM's limit (TOML.md)
  (t ("a = " + builtins.concatStringsSep "" (builtins.genList (i: if i < 16 then "[" else "{ b = ") 32)
      + "1" + builtins.concatStringsSep "" (builtins.genList (i: if i < 16 then " }" else "]") 32)))
]
