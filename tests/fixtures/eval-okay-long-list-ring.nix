# The window ring turned over between the elements of a walk (HEAP.md "Long
# lists", step 1). s is 3,204 small lists, built by fromJSON (in C: no
# bytecode, so cheap under stress) and promoted by churn. concatLists and
# deepSeq view each of them, 20 bytes a view, and fill the 60 KB ring unless
# they turn it over between elements.
let
  p = "[true,true,true,true],"; p2 = p + p; p4 = p2 + p2; p8 = p4 + p4; p16 = p8 + p8;
  p32 = p16 + p16; p64 = p32 + p32; p128 = p64 + p64; p256 = p128 + p128; p512 = p256 + p256;
  s = builtins.concatLists (builtins.genList (k: builtins.fromJSON "[${p512}${p256}${p32}[true]]") 4);
  churn = builtins.length (builtins.genList (x: x) 1400);   # 61 KB, its elements never forced
  s2 = builtins.seq s (builtins.seq churn s);
in [ (builtins.length (builtins.concatLists s2)) (builtins.deepSeq s2 true) ]
