# groupBy forces its function before its list: a throwing function, not the
# list that is no list, is the error.
builtins.groupBy (throw "f") 5
