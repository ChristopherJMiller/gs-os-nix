# orcarun -- runs ORCA shell programs (ORCA/C, the ORCA linker) on the host with no
# ROM and no GS/OS: the 65816 core shared with a2min plus a host implementation of
# the GS/OS, ORCA-shell and Toolbox calls those programs make. docs/TOOLCHAIN.md.
#
# Also `orcarun --omf2bin`, which turns an OMF load file into the flat binary a2min
# runs. Plain C++; no Verilator, so this builds in seconds.
{ lib
, stdenv
}:

stdenv.mkDerivation {
  pname = "orcarun";
  version = "0.1.0";

  src = lib.fileset.toSource {
    root = ../../sim;
    fileset = lib.fileset.unions [ ../../sim/orcarun.cpp ../../sim/cpu65816.h ];
  };

  buildPhase = ''
    runHook preBuild
    $CXX -O2 -std=c++17 -Wall -Wextra -o orcarun orcarun.cpp
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 orcarun "$out/bin/orcarun"
    runHook postInstall
  '';

  doInstallCheck = true;
  installCheckPhase = ''
    runHook preInstallCheck
    # with no arguments it prints its usage and exits 2 (stdenv runs with pipefail)
    ("$out/bin/orcarun" 2>&1 || true) | grep -q '^usage:'
    runHook postInstallCheck
  '';

  meta = {
    description = "Host harness that runs ORCA/C and the ORCA linker without an Apple IIgs";
    mainProgram = "orcarun";
    platforms = lib.platforms.unix;
  };
}
