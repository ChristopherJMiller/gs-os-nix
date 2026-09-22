# Cadius -- Brutal Deluxe's ProDOS/2mg disk image utility, mach-kernel's maintained
# fork. Absent from nixpkgs.
#
# Used alongside AppleCommander to build the disk images that carry the 65816 demo
# into MAME. Cadius is the better tool for ProDOS volumes and 2mg containers;
# AppleCommander is friendlier for DOS 3.3 and for scripted file insertion.
{ lib
, stdenv
, fetchzip
}:

stdenv.mkDerivation (finalAttrs: {
  pname = "cadius";
  version = "1.4.5";

  src = fetchzip {
    url = "https://github.com/mach-kernel/cadius/archive/refs/tags/"
      + "${finalAttrs.version}.tar.gz";
    hash = "sha256-s7fq5dwJCIeFgouCvJIKu41ir3IyiLt3UWcqjLHXaJc=";
  };

  # The Makefile's own `install` target hardcodes a DESTDIR layout and builds into
  # bin/release; just build the release target and place the binary directly.
  makeFlags = [ "release" "CC=${stdenv.cc.targetPrefix}cc" ];

  installPhase = ''
    runHook preInstall
    install -Dm755 bin/release/cadius $out/bin/cadius
    runHook postInstall
  '';

  doInstallCheck = true;
  # A smoke test only: the binary runs and says something. Cadius exits non-zero
  # with no arguments, so neither the exit code nor any particular banner text is
  # a reliable assertion.
  installCheckPhase = ''
    runHook preInstallCheck
    $out/bin/cadius > usage.txt 2>&1 || true
    test -s usage.txt || { echo "cadius produced no output at all"; exit 1; }
    head -3 usage.txt
    runHook postInstallCheck
  '';

  meta = with lib; {
    description = "ProDOS disk image utility for Apple II development";
    homepage = "https://github.com/mach-kernel/cadius";
    license = licenses.gpl2Plus;
    mainProgram = "cadius";
    platforms = platforms.unix;
  };
})
