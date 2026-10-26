# A derivation's drvPath is forced before its outPath.
builtins.toXML { type = "derivation"; drvPath = throw "drvPath"; outPath = throw "outPath"; }
