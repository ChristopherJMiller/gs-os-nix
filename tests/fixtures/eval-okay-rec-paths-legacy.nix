let r = rec { a = 1; b = a + 1; c.d = b + 1; c.e = 0; }; in [ r.b r.c.d r.c.e (let { body = 42; x = 1; }) { a.b.c = 1; a.b.d = 2; a = { e = 3; }; } ]
