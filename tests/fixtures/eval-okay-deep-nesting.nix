# The C stack is bank 0's top, $BFFF down to $7000, over the program
# (VM.md "The C stack", D31). Each kind of nesting below goes about
# nine-tenths as deep as the VM now holds, and each was E_NOMEM at a third
# of that when the stack was under the program. A call in an operand costs
# ~72 bytes of C stack a level (270 levels hold), a thunk forced inside a
# thunk ~130 (151), and a deepSeq of nested lists only ~21, so it is the
# root stack, RSTACK_N, that ends it (316). The root and force stacks are
# sized so that the C stack ends the first three: one root a level, and for
# a thunk one force.
let
  # not a tail call: the + waits for it
  count = n: if n == 0 then 0 else 1 + count (n - 1);
  # down a chain of nested sets
  nest = n: if n == 0 then 0 else { x = nest (n - 1); };
  depth = s: if s == 0 then 0 else 1 + depth s.x;
  # a lazy fold's accumulator: a thunk that forces the one before it
  foldl = op: z: l: if l == [] then z else foldl op (op z (builtins.head l)) (builtins.tail l);
  # lists in lists, forced all the way down
  lists = n: if n == 0 then [] else [ (lists (n - 1)) ];
in [
  (count 250)
  (depth (nest 250))
  (foldl (a: b: a + b) 0 (builtins.genList (i: i) 140))
  (builtins.deepSeq (lists 300) true)
]
