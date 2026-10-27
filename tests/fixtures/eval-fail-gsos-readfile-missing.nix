# A file that is not there: an I/O error tryEval does not catch.
builtins.tryEval (builtins.readFile ./gsos/missing)
