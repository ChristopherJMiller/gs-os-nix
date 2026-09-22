# a thunk created under a with keeps the with scope
let f = with { a = 5; }; x: a + x; s = with { b = 7; }; { inherit b; }; in [ (f 1) s.b ]
