# zipAttrsWith forces its function, though no value is used.
builtins.attrNames (builtins.zipAttrsWith 1 [ { a = 1; } ])
