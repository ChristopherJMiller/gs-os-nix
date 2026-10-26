# compareVersions, splitVersion, parseDrvName (Nix's libstore/names.cc) and
# replaceStrings (primops.cc), each element one rule, answered by the host.
let
  d = derivation { name = "d"; system = "s"; builder = "/b"; };
  cmp = builtins.compareVersions;
in [
  (cmp "1.0" "2.3") (cmp "2.1" "2.3") (cmp "2.3" "2.3") (cmp "2.5" "2.3") (cmp "3.1" "2.3")
  (cmp "2.3.1" "2.3") (cmp "2.3.1" "2.3a") (cmp "2.3pre1" "2.3") (cmp "2.3pre3" "2.3pre12")
  (cmp "2.3a" "2.3c") (cmp "2.3pre1" "2.3c") (cmp "2.3pre1" "2.3q") (cmp "1.2.3" "1.2.3.0")
  (cmp "a" "1") (cmp "1-2" "1.2") (cmp "" "") (cmp "1..2" "1.2") (cmp "1.2-pre" "1.2")
  (cmp "2147483647" "2147483648")              # past a C++ int a run of digits is text
  (cmp "007" "7") (cmp "1.b" "1.a") (cmp "1.z" "1.ab") (cmp "pre" "pre") (cmp "1pre" "1.pre")
  (builtins.splitVersion "1.2.3") (builtins.splitVersion "1.2a3-pre4") (builtins.splitVersion "")
  (builtins.splitVersion "..1--2") (builtins.splitVersion "abc") (builtins.splitVersion "1.2.3pre5")
  (builtins.parseDrvName "nix-0.12pre12876") (builtins.parseDrvName "a-b-c-1.0")
  (builtins.parseDrvName "hello") (builtins.parseDrvName "hello-") (builtins.parseDrvName "-1")
  (builtins.parseDrvName "a-B-2") (builtins.parseDrvName "foo-1-bar") (builtins.parseDrvName "")
  (builtins.replaceStrings [ "o" ] [ "a" ] "foobar")
  (builtins.replaceStrings [ "oo" "o" ] [ "u" "a" ] "foo")       # the first that matches
  (builtins.replaceStrings [ "o" "oo" ] [ "a" "u" ] "foo")
  (builtins.replaceStrings [ "" ] [ "X" ] "abc")                 # empty: everywhere
  (builtins.replaceStrings [ "" "a" ] [ "X" "Y" ] "ab") (builtins.replaceStrings [ "a" "" ] [ "Y" "X" ] "ab")
  (builtins.replaceStrings [ ] [ ] "abc") (builtins.replaceStrings [ "abc" ] [ "x" ] "ab")
  (builtins.replaceStrings [ "a" ] [ "aa" ] "aa")                # no rescan
  (builtins.replaceStrings [ "a" ] [ (throw "unused") ] "xyz")   # a `to` forced on use only
  (builtins.replaceStrings [ "" ] [ "-" ] "")
  (builtins.hasContext (builtins.replaceStrings [ "x" ] [ d.outPath ] "y"))
  (builtins.hasContext (builtins.replaceStrings [ "y" ] [ d.outPath ] "y"))
  (builtins.hasContext (builtins.replaceStrings [ d.outPath ] [ "z" ] "q"))   # from's context dropped
  (builtins.hasContext (builtins.replaceStrings [ "n" ] [ "m" ] "${d}"))
  (builtins.attrNames (builtins.getContext (builtins.replaceStrings [ "a" "b" ] [ d.outPath d.drvPath ] "ab")))
]
