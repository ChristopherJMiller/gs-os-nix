# nlohmann checks UTF-8 when it dumps the finished tree, after every value has
# been forced: the throw, not the earlier invalid string, is the error.
builtins.toJSON [ (builtins.substring 0 1 "é") (throw "after") ]
