# toString of an INT at each width boundary: prims.c int_to_str divides in 16
# bits when the value fits, 32 when it fits, 64 otherwise (ORCA/C's 64-bit divide
# is ~4,000 cycles). A wrong bound between the loops prints wrong digits here.
[ (toString 0) (toString 7) (toString 65535) (toString 65536) (toString (-65536)) (toString 4294967295) (toString 4294967296) (toString (-9223372036854775807 - 1)) (toString 9223372036854775807) (toString (-1)) ]
