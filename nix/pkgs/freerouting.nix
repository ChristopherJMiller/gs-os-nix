# SPDX-License-Identifier: GPL-3.0-or-later
#
# Freerouting 2.5.0 from the release jar (nixpkgs has 2.4.1). Its output is identical on
# every run, which the carrier's routing relies on (plan 1b, amendment 2). The jar is
# class file 69, so it runs on Java 25. Called as `pkgs.callPackage ./pkgs/freerouting.nix { }`.
{
  lib,
  stdenv,
  fetchurl,
  makeBinaryWrapper,
  jdk25_headless,
}:
stdenv.mkDerivation (finalAttrs: {
  pname = "freerouting";
  version = "2.5.0";

  src = fetchurl {
    url = "https://github.com/freerouting/freerouting/releases/download/v${finalAttrs.version}/freerouting-${finalAttrs.version}.jar";
    sha256 = "f6f51bb02245e8e717f9359bd260cc9c5c0b1bc0acc8b7cb2cd5b8ffeb5de3c7";
  };

  dontUnpack = true;
  nativeBuildInputs = [ makeBinaryWrapper ];

  installPhase = ''
    runHook preInstall
    mkdir -p $out/{bin,share/freerouting}
    cp $src $out/share/freerouting/freerouting-${finalAttrs.version}.jar
    makeWrapper ${lib.getExe jdk25_headless} $out/bin/freerouting \
      --add-flags "-jar $out/share/freerouting/freerouting-${finalAttrs.version}.jar"
    runHook postInstall
  '';

  meta = {
    description = "Advanced PCB auto-router (release jar)";
    homepage = "https://www.freerouting.org";
    license = lib.licenses.gpl3Only;
    sourceProvenance = [ lib.sourceTypes.binaryBytecode ];
    platforms = lib.platforms.linux ++ lib.platforms.darwin;
    mainProgram = "freerouting";
  };
})
