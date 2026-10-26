# builtins.match and builtins.split: libstdc++'s std::regex in extended
# syntax, as Nix 2.34 runs it (docs/REGEX.md). Each answer is one rule.
let
  ctx = builtins.toFile "regex-ctx" "x";
  inner = builtins.match "x*" "xx";               # a match run while forcing the string
  long = builtins.concatStringsSep "" (builtins.genList (_: "ab") 1000);
in [
  # the depth-first executor
  (builtins.match "(a|ab)(c|bcd)(d*)" "abcd")     # the first whole match it finds
  (builtins.split "(a|ab)" "abc")                 # a search keeps the longest
  (builtins.match "((a)|b)*" "ab")                # a group keeps an earlier iteration's capture
  (builtins.match "(a*)*" "aa")                   # the last iteration, an empty one
  (builtins.match "((^)|a)*" "a")                 # a loop entered twice where it was keeps that capture
  (builtins.match "(a*)+" "b")
  (builtins.match "(a)|(b)" "b")                  # a group that took no part is null
  (builtins.match "(a|b)*c" "abac")
  (builtins.split "(a*)(ab)?" "aab")              # a loop that matched never tries its earlier exits
  (builtins.split "(a|ab)(b?)" "ab")              # of two as long, the first
  (builtins.match "(a)b|ac" "ac")                 # a group's end is undone with its arm
  (builtins.match "a" "ab")                       # all of the string, or null
  (builtins.split "(x*)" "axb")                   # after an empty match: not empty here, or one on
  (builtins.split "" "ab")
  (builtins.split "^a" "aaa")                     # ^ only where the string starts
  (builtins.split "a$" "aaa")
  (builtins.split "(a)|b" "ab")                   # each match's groups start afresh
  (builtins.split "," "a,b,,c")
  # the syntax
  (builtins.match "[]a]+" "]a]")                  # a ] first in a bracket is a character
  (builtins.match "[^]a]" "b")
  (builtins.match "[a-]+" "a-")
  (builtins.match "[--/]+" "-./")
  (builtins.match "[[:alpha:]_][[:alnum:]_]*" "foo_1")
  (builtins.match "[[:ALPHA:]]" "a")              # a class name is lowercased
  (builtins.match "[[:w:]]+[[:d:]]" "a_1")        # and w, d, s are classes too
  (builtins.match "[[=a=]]+" "aA")                # an equivalence class is tolower's
  (builtins.match "[[.hyphen.]][[.space.]]" "- ")
  (builtins.match "a{2}b{1,}c{0,1}" "aabbc")
  (builtins.match "(a){2}" "aa")                  # one group, twice
  (builtins.match "a{3}{2}" "aaaaaa")
  (builtins.match "a**" "aa")
  (builtins.match "a*?" "aa")                     # ? after * is another quantifier, not lazy
  (builtins.match "\\.\\*\\\\" ".*\\")            # only a special character is escaped
  (builtins.match "[\\]" "\\")                    # a backslash in a bracket is itself
  (builtins.match "a]}" "a]}")
  (builtins.match "a|" "")
  (builtins.match "()" "")
  # bytes: char is signed, and the "C" locale's classes are ASCII
  (map builtins.stringLength (builtins.match "(.)(.)" "é"))
  (builtins.match "caf[^a-z]+" "café")
  (builtins.match "[[:alpha:]]+" "é")
  # the values
  (map builtins.hasContext (builtins.split "Q" "${ctx}"))   # no match: the string itself
  (map builtins.hasContext (builtins.filter builtins.isString (builtins.split "/" "${ctx}")))
  (map builtins.hasContext (builtins.match "(.*)" "${ctx}"))
  (builtins.match "(a)(b)" (if inner != null then "ab" else ""))
  (builtins.length (builtins.split "," (builtins.concatStringsSep "," (builtins.genList toString 300))))
  (map builtins.stringLength (builtins.match "(.*)(b)" long))
]
