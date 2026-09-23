# ORCA/C -- Byte Works' C compiler for the Apple IIgs, from its official GitHub
# release. DECISIONS.md D18: the only gratis native-mode 65816 C compiler, and the
# freest that exists (source on GitHub, use/fork/modify permitted, redistribution
# not). Nothing here is redistributed: every machine fetches the release itself,
# pinned by hash, exactly as nixpkgs handles unfree packages.
#
# The release is a ProDOS-order .2mg. cadius extracts it into a host tree whose
# file names carry the ProDOS type and auxtype as `name#TTAAAA` -- the convention
# sim/orcarun.cpp reads back, so `Languages/cc#B5DB01` is an EXE ($B5) with
# auxtype $DB01 (GS/OS aware, wants long prefixes).
#
# $out/orca is what `orcarun --orca` expects: Languages/ (cc, Linker), Libraries/
# (ORCALib, SysLib, SysFloat, ORCACDefs/*.h), Utilities/, Release.Notes/.
{ lib
, stdenvNoCC
, fetchurl
, cadius
}:

stdenvNoCC.mkDerivation (finalAttrs: {
  pname = "orcac";
  version = "2.3.0b1";

  src = fetchurl {
    url = "https://github.com/byteworksinc/ORCA-C/releases/download/orcac-230b1/ORCAC.230B1.2mg";
    hash = "sha256-6svoz7G+wJcg7CPo6D5AQvXw1kN+wHk/zgx3Z4ytRC4=";
  };

  dontUnpack = true;
  nativeBuildInputs = [ cadius ];

  installPhase = ''
    runHook preInstall
    mkdir -p extracted
    cadius EXTRACTVOLUME "$src" extracted/ > extract.log 2>&1 \
      || { cat extract.log; exit 1; }
    grep -q 'Error(s) : 0' extract.log || { cat extract.log; exit 1; }
    mkdir -p "$out"
    mv extracted/ORCAC.230B1 "$out/orca"
    # The two binaries the harness runs, and the headers the compiler needs.
    test -f "$out/orca/Languages/cc#B5DB01"
    test -f "$out/orca/Languages/Linker#B5DB01"
    test -f "$out/orca/Libraries/ORCACDefs/stdint.h#B00008"
    test -f "$out/orca/Libraries/ORCALib#B20000"
    runHook postInstall
  '';

  meta = {
    description = "ORCA/C 2.3.0 B1, the Apple IIgs C compiler, with its linker, libraries and headers";
    homepage = "https://github.com/byteworksinc/ORCA-C";
    # LICENSE in the repository: "grants you the right to use this source code
    # privately, fork it, and change it. You may not redistribute the code in any
    # form other than submission to this repository".
    license = {
      shortName = "byteworks-orca";
      fullName = "Byte Works ORCA licence: private use, forking and modification; no redistribution";
      url = "https://github.com/byteworksinc/ORCA-C/blob/main/LICENSE";
      free = false;
      redistributable = false;
    };
    platforms = lib.platforms.all;
  };
})
