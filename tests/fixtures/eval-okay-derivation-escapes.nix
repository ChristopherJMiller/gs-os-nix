# derivationStrict's attribute rules and the ATerm's quoting, each measured
# against the host (docs/DERIVATION.md):
#  - builder, args and environment values are escaped: " \ newline CR tab;
#  - `system` is NOT (printUnquotedString): the .drv this makes is one Nix
#    cannot parse back, yet a read-only evaluation reports its path, and so
#    must the VM;
#  - args coerce with toString's rules: 1, true -> "1", false and null -> "",
#    a list joins with spaces but none after an empty list, a set through
#    outPath or __toString;
#  - two outputs, sorted, the second's path named "edge-dev";
#  - __structuredAttrs = false stays in the environment, __contentAddressed =
#    false does not.
let
  d = derivation {
    name = "edge";
    system = "x86\"64";
    builder = "/bin/sh\t\"q\"\\";
    args = [ "-c" 1 true false null [ "a" [ ] "b" ] { outPath = "/o"; } { __toString = s: "ts"; } ];
    outputs = [ "out" "dev" ];
    multi = "line1\nline2\r";
    flag = true;
    nothing = null;
    num = -42;
    lst = [ 1 [ ] 2 ];
    __structuredAttrs = false;
    __contentAddressed = false;
  };
in [ d.drvPath d.outPath d.dev.outPath d.out.outputName d.dev.outputName (builtins.attrNames d) ]
