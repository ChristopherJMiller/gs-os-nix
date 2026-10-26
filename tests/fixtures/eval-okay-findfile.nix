# builtins.findFile, and <x>, which is __findFile __nixPath "x" (D26: nixPath
# is [ ] here): the list, its elements, their prefix and path forced before
# the name; a name not found is a throw, which tryEval catches.
[
  (builtins.tryEval (builtins.findFile [ ] "x"))
  (builtins.tryEval (builtins.findFile [ { prefix = "a"; path = "/nonexistent"; } ] "b/c"))
  (builtins.tryEval (builtins.findFile [ { prefix = "a"; path = "/nonexistent"; } ] "ab"))
  (let __nixPath = [ ]; in builtins.tryEval <x>)
  (let __findFile = p: n: n; in <some/thing>)
  (builtins.typeOf builtins.nixPath)
  (builtins.tryEval (builtins.findFile (throw "list") "x"))
  (builtins.tryEval (builtins.findFile [ (throw "element") ] "x"))
  (builtins.tryEval (builtins.findFile [ { path = throw "path"; } ] "x"))
  (builtins.tryEval (builtins.findFile [ { prefix = throw "prefix"; path = "/p"; } ] "x"))
  (builtins.tryEval (builtins.findFile [ ] (throw "name")))
]
