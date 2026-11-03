# The Verilated a2nix_card, packaged as a static library plus headers.
#
# This exists so MAME can link the RTL rather than reimplement it. The RTL stays
# the single source of truth: MAME and a real ECP5 bitstream run identical logic
# (docs/ARCHITECTURE.md).
#
# Verilator's own `--build` archives only the generated model, not its runtime
# (verilated.cpp et al), because that is normally pulled in by `--exe`. We are not
# building an executable here, so the runtime is compiled and archived alongside.
# (Verilator 5 leaves its own runtime objects in obj/ too; only the model's are
# taken from there, or the archive holds each runtime object twice and a link
# fails on "multiple definition".)
{ lib
, stdenv
, verilator
, python3
, rtlSrc
, topModule ? "a2nix_card"
}:

stdenv.mkDerivation {
  pname = "a2nix-verilated";
  version = "0.1.0";

  src = rtlSrc;

  # python3 is required by verilator's own verilator_includer, not by us.
  nativeBuildInputs = [ verilator python3 ];

  dontConfigure = true;

  buildPhase = ''
    runHook preBuild

    verilator --cc --build -j $NIX_BUILD_CORES \
      -I. --top-module ${topModule} \
      -Wall -Wno-DECLFILENAME \
      --Mdir obj \
      ${topModule}.v > verilator.log 2>&1 || { cat verilator.log; exit 1; }
    cat verilator.log

    # The VM_* settings the model was compiled with, from the compile lines
    # verilator's own make ran. verilated.h tests each with #if, so a consumer
    # built with -Wundef -Werror, as MAME is, has to define them, and with the
    # model's values: nix-support/cflags.
    grep -o -- '-DVM_[A-Z_]*=[0-9]*' verilator.log | sort -u | tr '\n' ' ' > vm-cflags
    grep -q -- '-DVM_SC=' vm-cflags \
      || { echo "no -DVM_SC= in verilator's compile lines"; exit 1; }

    VROOT=$(verilator --getenv VERILATOR_ROOT)
    VINC="$VROOT/include"

    for f in verilated verilated_threads; do
      $CXX -std=c++20 -Os -fPIC -c $(cat vm-cflags) \
        -I"$VINC" -I"$VINC/vltstd" -Iobj \
        "$VINC/$f.cpp" -o "$f.o"
    done

    # One archive holding the model and the runtime it needs.
    $AR rcs liba2nixcard.a obj/V${topModule}*.o verilated.o verilated_threads.o

    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall

    mkdir -p $out/include $out/lib $out/nix-support
    cp obj/*.h $out/include/
    cp liba2nixcard.a $out/lib/
    cp vm-cflags $out/nix-support/cflags

    runHook postInstall
  '';

  # Consumers also need verilator's own headers (verilated.h and friends), so
  # expose the path rather than making each of them re-derive it.
  passthru.verilatorInclude = "${verilator}/share/verilator/include";

  meta = with lib; {
    description = "Verilated a2nix accelerator card, as a static library for MAME";
    platforms = platforms.unix;
  };
}
