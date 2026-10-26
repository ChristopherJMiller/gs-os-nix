# A to is forced where it is first used: at "b", the start of "ba".
builtins.replaceStrings [ "a" "b" ] [ (throw "for a") (throw "for b") ] "ba"
