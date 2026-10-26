# builtins.fromJSON as Nix 2.34 reads JSON (json-to-value.cc through
# nlohmann/json), each element one rule, answered by the host; and toJSON's
# escapes for the control characters only fromJSON can make.
let j = builtins.fromJSON; in [
  (j "null") (j "true") (j "false") (j "0") (j "-0") (j "17") (j "-17")
  (j "9223372036854775807") (j "-9223372036854775808")
  (j "18446744073709551616") (j "-9223372036854775809")     # too wide: floats
  (j "123456789012345678901234567890")
  (j "1.0") (j "-0.0") (j "0.1") (j "1e2") (j "1E-2") (j "-0e0") (j "0.1e1") (j "1.5e-3")
  (j "0.30000000000000004") (j "4.9e-324") (j "1e-400") (j "2.2250738585072012e-308")
  (j "1.7976931348623157e308") (j "9007199254740993")
  (j "1.00000000000000011102230246251565404236316680908203125" == 1.0)    # a tie: to even
  (j "1.000000000000000111022302462515654042363166809082031251" == 1.0)   # just above: up
  (j ''"plain"'') (j ''"esc \" \\ \/ \b \f \n \r \t"'') (j ''"\u00e9\u00E9 \ud83d\ude00 \u4e2d"'')
  (j ''"é 日本"'')
  (j "[]") (j "{}") (j " [ 1 , 2 ] ") (j "[[[[[[[[[[1]]]]]]]]]]") (j ''{"b":[{"c":null}],"a":true}'')
  (j ''{"a":1,"a":2}'') (j ''{"a":1,"b":2,"a":3}'') (j ''{"":0," ":1}'')
  (j "﻿[1]")
  (builtins.toJSON (j ''"\b\f\u0001\u001f\u007f"''))
  (builtins.toJSON (j ''{"k":[1,2.5,"x",null,{"y":false}]}''))
  (j (builtins.toJSON { a = [ 1 2.5 "s" null true ]; b = { c = -3; }; }))
  (builtins.typeOf (j "1")) (builtins.typeOf (j "1.0")) (builtins.typeOf (j "1e0"))
]
