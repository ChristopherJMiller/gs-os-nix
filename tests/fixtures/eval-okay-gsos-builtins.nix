# The builtins that need GS/OS are values like any other (DECISIONS.md D26):
# builtins.NAME, __NAME, and the fetchers unqualified. Each forces its
# arguments before it would touch a file, so a throw in one is caught.
[
  (builtins.typeOf builtins.readFile)
  (builtins.typeOf __pathExists)
  (builtins.typeOf fetchTarball)
  (builtins.typeOf __hashFile)
  (builtins.tryEval (builtins.readFile (throw "the path")))
  (builtins.tryEval (builtins.hashFile "sha256" (throw "the path")))
  (builtins.tryEval (__getEnv (throw "the name")))
  (let __readFile = x: x + 1; in __readFile 1)   # a __ name is a variable like any other
  (builtins.isFunction builtins.filterSource)
]
