# An attribute inserted out of order moves the ones after it, and each keeps
# its own position: listToAttrs's and a computed name's insertions (HEAP.md
# "ATTRS is struct-of-arrays"). Its own file, as eval-okay-attrpos is near
# the VM's 256 chunks.
let
  pos = n: s: let p = builtins.unsafeGetAttrPos n s; in [ p.line p.column (baseNameOf p.file) ];
in [
  (pos "c" (builtins.listToAttrs [ { name = "c"; value = 1; } { name = "b"; value = 2; } ]))
  (pos "b" (builtins.listToAttrs [ { name = "c"; value = 1; } { name = "b"; value = 2; } ]))
  (pos "c" { ${"c"} = 1; ${"b"} = 2; })
  (pos "b" { ${"c"} = 1; ${"b"} = 2; })
  (pos "z" { z = 0; ${"c"} = 1; ${"a"} = 2; })
]
