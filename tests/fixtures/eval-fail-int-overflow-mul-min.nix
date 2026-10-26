# The minimum times -1 is 2^63: both operands negative, so the limit is
# 2^63 - 1, not 2^63 (which a negative product may reach).
(-9223372036854775807 - 1) * -1
