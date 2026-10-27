# readFile through GS/OS (GSOS.md): a file, then through symlinks the VM
# resolves itself as Nix does -- a chain, a target with . and .., a link that
# goes back up through another -- and the argument as coerceToPath has it: a
# path, an absolute string made canonical, a set's __toString and outPath.
let r = builtins.readFile; in [
  (r ./gsos/text)
  (r ./gsos/link-to-file)
  (r ./gsos/chain)
  (r ./gsos/up-and-back)
  (r ./gsos/dir/via-parent)
  (r (toString ./gsos/text))
  (r (toString ./gsos + "//./dir/../text"))
  (r { __toString = _: toString ./gsos/dir/inner; })
  (r { outPath = toString ./gsos/link-to-dir/inner; })
]
