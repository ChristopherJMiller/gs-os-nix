# No FST has a ":" in a name: refused by name, before GS/OS is asked.
builtins.readFile (toString ./gsos + "/a:b")
