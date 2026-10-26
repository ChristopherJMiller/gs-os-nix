# A signed compare at the extremes. (Its first version also asserted that
# max + 1 wraps to min; Nix 2.34 makes that an error, eval-fail-int-overflow-add.)
(-9223372036854775807 - 1 < 9223372036854775807) && (-1 < 0) && (0 < 1) && !(-9223372036854775807 - 1 > 0) && (9223372036854775807 > -1)
