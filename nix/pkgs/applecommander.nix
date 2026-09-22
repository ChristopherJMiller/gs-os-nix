# AppleCommander -- manipulate Apple II disk images (ProDOS, DOS 3.3, ...).
#
# Needed to get the 65816 demo binary into a disk image MAME can boot, which is
# the tidy route to M2. Absent from nixpkgs.
#
# The platform-independent `ac` CLI jar is used rather than the per-platform
# native-image builds, so this derivation stays the same on every system.
{ lib
, stdenvNoCC
, fetchurl
  # 14.1's jar is class file version 69, i.e. Java 25. The default `jre` is 21 and
  # fails with UnsupportedClassVersionError, so pin the headless JDK 25.
, jdk25_headless
, makeWrapper
}:

stdenvNoCC.mkDerivation (finalAttrs: {
  pname = "applecommander";
  version = "14.1";

  src = fetchurl {
    url = "https://github.com/AppleCommander/AppleCommander/releases/download/"
      + "${finalAttrs.version}/AppleCommander-ac-${finalAttrs.version}.jar";
    hash = "sha256-/oK57R6eFbJYY7w14lAV2nzjAQexdityddRSjZDmmM4=";
  };

  dontUnpack = true;

  nativeBuildInputs = [ makeWrapper ];

  installPhase = ''
    runHook preInstall

    mkdir -p $out/share/java $out/bin
    install -Dm444 $src $out/share/java/AppleCommander.jar

    makeWrapper ${jdk25_headless}/bin/java $out/bin/ac \
      --add-flags "-jar $out/share/java/AppleCommander.jar"

    runHook postInstall
  '';

  doInstallCheck = true;
  # `ac` exits non-zero with no arguments, so the exit code proves nothing. Assert
  # the absence of a Java exception instead: an earlier version of this check only
  # required non-empty output, and happily passed on an
  # UnsupportedClassVersionError stack trace.
  installCheckPhase = ''
    runHook preInstallCheck
    $out/bin/ac > usage.txt 2>&1 || true
    test -s usage.txt || { echo "ac produced no output at all"; exit 1; }
    if grep -qE "Exception|Error:" usage.txt; then
      echo "ac failed to start:"
      head -5 usage.txt
      exit 1
    fi
    head -3 usage.txt
    runHook postInstallCheck
  '';

  meta = with lib; {
    description = "Apple II disk image manipulation tool";
    homepage = "https://applecommander.github.io/";
    license = licenses.gpl2Plus;
    mainProgram = "ac";
    platforms = platforms.unix;
  };
})
