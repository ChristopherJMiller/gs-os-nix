# Every 64-bit CPU-side operation must carry across all four words.
# The first VM counted its word loops with CPY, which clobbers the carry.
let a = 65535 + 1; b = 65536 - 1; c = 0 - 65536; d = -c; e = 4294967295 + 1; in a * 1000000 + b + c + d + e - 4294967296
