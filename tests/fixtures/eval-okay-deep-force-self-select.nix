# A set whose attribute selects from the set itself, deep-forced: printing it
# forces `c`, whose `a.b` is a SELECT on the set being visited -- marked (bit 23
# of aux, HEAP.md). Under GC stress the set is promoted, so that SELECT is the
# card's LOOKUP, which used to count the mark and read `b`'s value from 32 MB
# past the set (null here: E_BADNBC). tvix's fix, rec-nested-access and
# legacy-let-fix failed the same way.
let
  a = { b = 1; c = a.b + 1; d = [ a.b a.c ]; };
  l = [ 1 (builtins.elemAt l 0) ];
  fix = f: let x = f x; in x;
in [
  a
  (builtins.deepSeq a a.c)
  l
  (fix (self: { x = 2; y = self.x * 3; z = { w = self.y; }; }))
  (rec { p.q = 1; p.r = p.q * 2; })
]
