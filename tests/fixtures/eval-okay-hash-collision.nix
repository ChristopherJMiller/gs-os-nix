# "getxolul" and "wswmfewd" have the same djb2 key hash (0x003A8719), so the
# card's CAM matches both and returns the lower index -- the wrong attribute,
# since attributes are sorted by name and "getxolul" sorts first. Looking up
# "wswmfewd" is therefore the one case that needs find_attr's CPU-scan
# fallback (docs/HEAP.md, "Attribute lookup on a mature set"). It only reaches
# that path once the set is mature, so this fixture earns its keep under
# --run-nbc-gc-stress, where every boundary promotes.
let s = { getxolul = 1; wswmfewd = 2; }; t = s // { qq = 3; };
in [ s.getxolul s.wswmfewd (s ? getxolul) (s ? wswmfewd) (s.nope or 7)
     t.wswmfewd t.getxolul (builtins.attrNames s) (s == { wswmfewd = 2; getxolul = 1; }) ]
