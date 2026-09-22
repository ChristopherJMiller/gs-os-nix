# The test-only .nbc fixture generator (tools/nbc). DECISIONS.md D7: never a
# runtime component; D16: Rust, because it does not touch the Verilated model.
#
# Building it runs `cargo test`, so `checks.nbc-tests = ours.nbc` is the unit-test
# gate. No dependencies, so the lockfile is trivial and checks.fixture-purity can
# grep the manifest for crypto crates.
{ rustPlatform, lib }:

rustPlatform.buildRustPackage {
  pname = "nbc";
  version = "0.1.0";
  src = lib.cleanSource ../../tools/nbc;
  cargoLock.lockFile = ../../tools/nbc/Cargo.lock;

  meta = {
    description = "Test-only .nbc bytecode generator for the gs-os-nix 65816 VM";
    mainProgram = "nbc";
  };
}
