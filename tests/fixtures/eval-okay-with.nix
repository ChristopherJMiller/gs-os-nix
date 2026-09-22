let x = 1; in with { y = 2; z = 3; }; with { z = 30; q = 4; }; [ x y z q (map (a: a * 2) [ 1 ]) ]
