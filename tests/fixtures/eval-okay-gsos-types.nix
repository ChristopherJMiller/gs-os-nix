# readFileType is the last component's own type (a link is "symlink"), with
# nothing resolved (a link among the ancestors is an error: the eval-fail
# fixture); readDir lists each entry's own type, the directory itself reached
# through every link.
let t = builtins.readFileType; in [
  (t ./gsos/text) (t ./gsos/dir) (t ./gsos/link-to-file) (t ./gsos/link-to-dir)
  (t ./gsos/dangling) (t ./gsos/loop-a) (t ./gsos/dir/via-parent)
  (builtins.readDir ./gsos)
  (builtins.readDir ./gsos/dir)
  (builtins.readDir ./gsos/link-to-dir)
]
