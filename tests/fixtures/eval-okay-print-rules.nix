# What the printer decides, each rule visible in the answer (COMPILER.md
# "Stage 4"): a set or list printed before is «repeated», wherever it comes
# again; a keyword or a non-identifier is quoted as a name, `or`, ' and - are
# not; "${" is escaped in a string and a lone $ is not; empty and nested
# containers; negative numbers; a path is raw (absolute, so the host prints
# it as the VM does).
let
  shared = { x = 1; };
  pair = [ 1 2 ];
in {
  repeated = [ shared shared { inner = shared; } pair [ pair ] ];
  names = { "if" = 1; "in" = 2; "inherit" = 3; "or" = 4; "a'b" = 5; "a-b" = 6; "a b" = 7; "1x" = 8; "_y" = 9; "" = 10; "é" = 11; };
  strings = [ "a\${b}" "$" "$$" "a$b" "\n\t\r\"\\" "''" ];
  nested = [ [ ] { } [ [ ] ] { a = { }; } ];
  numbers = [ (-1) (-9223372036854775807 - 1) (-1.5) 1.0e-5 123456789.0 ];
  paths = [ /a/b /a/../c/./d ];
}
