# sort forces every element, in order, before it compares any: the throwing
# element, not the throwing comparator, is the error.
builtins.sort (a: b: throw "comparator") [ 1 (throw "element") ]
