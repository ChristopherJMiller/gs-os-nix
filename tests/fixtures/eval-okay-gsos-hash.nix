# hashFile hashes a file's bytes, NULs and all (only readFile refuses a NUL),
# through its links, with each algorithm hashString has.
map (algo: map (builtins.hashFile algo) [ ./gsos/binary ./gsos/text ./gsos/chain ]) [ "md5" "sha1" "sha256" "sha512" ]
