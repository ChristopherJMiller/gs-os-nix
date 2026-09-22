let a = { x = 1; y = { z = "q"; }; "two words" = true; }; in [ a.x a.y.z (a.y.w or 0) (a ? y.z) (a ? nope) ((a // { x = 2; w = 3; }).x) (a // { w = 3; }) a."two words" ]
