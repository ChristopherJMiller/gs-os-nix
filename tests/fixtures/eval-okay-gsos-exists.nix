# pathExists: the last component's link is not followed (a dangling link, a
# loop, exist), unless the argument is a string ending in "/" or "/.", which
# must then be a directory, every link followed.
let e = builtins.pathExists; s = toString ./gsos; in [
  (e ./gsos/text) (e ./gsos/missing) (e ./gsos/dir) (e ./gsos/missing/deeper)
  (e (s + "/dir/")) (e (s + "/dir/.")) (e (s + "/text/")) (e (s + "/text/."))
  (e (s + "/link-to-dir/")) (e (s + "/link-to-file/"))
  (e ./gsos/dangling) (e (s + "/dangling/")) (e ./gsos/loop-a)
  (e ./gsos/link-to-dir/inner) (e ./gsos/dir/via-parent) (e ./gsos/text/x)
  (e "/dev/null/.")
]
