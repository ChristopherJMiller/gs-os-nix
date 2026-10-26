# floor and ceil must land in [-2^63, 2^63) (docs/FLOAT.md): 2^63 itself
# is out, one past the largest INT.
builtins.ceil 9223372036854775807.0
