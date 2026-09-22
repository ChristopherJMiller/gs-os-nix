let f = { a, b ? a + 1, ... } @ args: [ a b (args ? c) ]; g = { x }: x; in [ (f { a = 1; }) (f { a = 1; b = 5; c = 0; }) (g { x = "s"; }) ]
