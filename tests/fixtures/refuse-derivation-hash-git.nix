# outputHashMode = "git" is Nix behind the git-hashing experimental feature (the
# host, without it, rejects this): its fixed path is "fixed:out:git:...". Refused
# rather than hashed as flat.
(derivation { name = "f"; system = "s"; builder = "b"; outputHash = "sha256-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU="; outputHashMode = "git"; }).outPath
