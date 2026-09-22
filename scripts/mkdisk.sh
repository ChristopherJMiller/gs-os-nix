#!/usr/bin/env bash
# Build a ProDOS disk image carrying the demo binary.
#
# This is the INTERACTIVE route: attach the image to MAME's IIgs SmartPort and
# BRUN the binary from BASIC, so you watch the store path appear on a real text
# screen. The automated M2 check uses the debugger instead (scripts/run-m2.sh),
# because booting an OS adds a dependency that can fail for reasons unrelated to
# the card.
#
# Note on what we are NOT doing: we are not making the a2nix card itself the
# storage device. That is the SmartPort storage card rejected in DECISIONS.md D1,
# and here it would also be circular -- the card would have to work in order to
# load the program that tests whether the card works.
#
# The image is a bare ProDOS volume with no system files, so it is not bootable
# on its own. Either add ProDOS/GS-OS yourself (`ac -p` or cadius ADDFILE), or
# attach it as a second device alongside a system disk you already have.
set -euo pipefail

# NB: these are deliberately prefixed. nixpkgs' stdenv exports SIZE, AR, NM,
# STRIP and friends as binutils tool names, so a plain `SIZE=${SIZE:-800KB}`
# silently inherits "size" inside `nix develop` and cadius rejects it.
A2_BIN=${A2_BIN:-target/build/storepath_demo.bin}
A2_OUT=${A2_OUT:-target/build/a2nix.2mg}
A2_VOLNAME=${A2_VOLNAME:-A2NIX}
A2_VOLSIZE=${A2_VOLSIZE:-800KB}   # cadius wants a sized string; block counts are rejected
STAGE=target/build/diskstage

command -v cadius >/dev/null || { echo "cadius not on PATH (use: nix develop)" >&2; exit 1; }
[ -f "$A2_BIN" ] || { echo "missing $A2_BIN (build it with: just asm)" >&2; exit 1; }

# Cadius takes ProDOS type and auxtype from the FILENAME, as NAME#TTAAAA. Type
# $06 is BIN and auxtype is the load address -- $2000 here, matching
# target/tests/apple2.cfg. Getting the auxtype wrong makes BRUN load to the wrong
# address and crash, so these two must stay in step.
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp "$A2_BIN" "$STAGE/STOREPATH#062000"

rm -f "$A2_OUT"
cadius CREATEVOLUME "$A2_OUT" "$A2_VOLNAME" "$A2_VOLSIZE" --quiet
cadius ADDFILE "$A2_OUT" "/$A2_VOLNAME/" "$STAGE/STOREPATH#062000" --quiet

echo "=== $A2_OUT ==="
cadius CATALOG "$A2_OUT" | sed 's/^/  /'
cat <<EOF

Attach it to MAME's IIgs SmartPort and BRUN it:

  mame apple2gs -sl4 a2nix -flop3_5_1 $A2_OUT \\
      -autoboot_delay 4 -autoboot_command 'BRUN STOREPATH\\n'

Expected output: the 32 characters from tests/vectors/l1.txt.
EOF
