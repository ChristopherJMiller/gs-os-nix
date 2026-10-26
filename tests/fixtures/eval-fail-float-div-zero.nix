# A float divided by zero is an error, as in Nix (prim_div tests the divisor
# as a float): here a negative zero, which a bit test for 0 would miss.
1.5 / (0.0 * -1)
