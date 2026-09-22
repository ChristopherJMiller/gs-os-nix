# MAME with the a2nix accelerator card added to the Apple IIgs slot options.
#
# The card's behaviour is the Verilated RTL (nix/pkgs/verilated-card.nix); this
# override only teaches MAME's build about mame/a2nix.{cpp,h} and registers the
# slot option.
#
# Patching is done with anchored edits rather than a static .patch file, so a MAME
# version bump produces a clear failure at a named anchor instead of a rejected
# hunk. Every edit is VERIFIED afterwards -- silently building a MAME without the
# card would be far worse than failing here, because the symptom would be a
# confusing "no such slot option" much later.
{ lib
, mame
, verilatedCard
, cardSrc
}:

mame.overrideAttrs (old: {
  pname = "mame-a2nix";

  postPatch = (old.postPatch or "") + ''
    echo "--- injecting a2nix card into MAME ---"

    cp ${cardSrc}/a2nix.cpp ${cardSrc}/a2nix.h src/devices/bus/a2bus/

    # 1. Add our sources to the A2BUS file list in the build script.
    anchor='MAME_DIR .. "src/devices/bus/a2bus/a2bus.cpp",'
    grep -qF "$anchor" scripts/src/bus.lua \
      || { echo "ANCHOR LOST: a2bus.cpp in scripts/src/bus.lua"; exit 1; }
    sed -i "s|$anchor|&\n\t\tMAME_DIR .. \"src/devices/bus/a2bus/a2nix.cpp\",\n\t\tMAME_DIR .. \"src/devices/bus/a2bus/a2nix.h\",|" \
      scripts/src/bus.lua

    # 2. Include our header in the card registry.
    cards=src/devices/bus/a2bus/cards.cpp
    test -f "$cards" || { echo "MISSING: $cards"; exit 1; }
    sed -i '0,/^#include "/s||#include "a2nix.h"\n&|' "$cards"

    # 3. Register the slot option for the IIgs. Insert immediately after the
    #    opening brace of apple2gs_cards().
    awk '
      /^void apple2gs_cards\(device_slot_interface &device\)/ { found = 1 }
      { print }
      found && /^\{/ && !done {
        print "\tdevice.option_add(\"a2nix\", A2BUS_A2NIX);  // gs-os-nix accelerator"
        done = 1
      }
    ' "$cards" > "$cards.new" && mv "$cards.new" "$cards"

    # --- Verify all three edits landed ---
    grep -qF 'a2bus/a2nix.cpp' scripts/src/bus.lua \
      || { echo "VERIFY FAILED: a2nix.cpp not in bus.lua"; exit 1; }
    grep -qF '#include "a2nix.h"' "$cards" \
      || { echo "VERIFY FAILED: a2nix.h not included in cards.cpp"; exit 1; }
    grep -qF 'A2BUS_A2NIX' "$cards" \
      || { echo "VERIFY FAILED: slot option not registered in cards.cpp"; exit 1; }
    # The option must be inside apple2gs_cards(), not merely somewhere in the file.
    awk '/^void apple2gs_cards/,/^\}/' "$cards" | grep -qF 'A2BUS_A2NIX' \
      || { echo "VERIFY FAILED: A2BUS_A2NIX not inside apple2gs_cards()"; exit 1; }

    echo "--- a2nix card injected and verified ---"
  '';

  makeFlags = (old.makeFlags or [ ]) ++ [
    # Headers for the generated model and verilator's runtime.
    "ARCHOPTS=-I${verilatedCard}/include -I${verilatedCard.verilatorInclude}"
    # The model itself.
    "LDOPTS=-L${verilatedCard}/lib -la2nixcard"
  ];

  meta = (old.meta or { }) // {
    description = "MAME with the gs-os-nix a2nix accelerator card for the Apple IIgs";
  };
})
