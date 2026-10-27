# getEnv reads the ORCA shell's variables (D30): TEST_VAR is foo, as Nix's
# tests/functional/lang.sh exports it; an unset one is "".
[ (builtins.getEnv "TEST_VAR") (builtins.getEnv "GS_OS_NIX_NO_SUCH_VAR") ]
