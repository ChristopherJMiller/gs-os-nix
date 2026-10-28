# SPDX-License-Identifier: GPL-3.0-or-later
"""Mechanical facts the boards are built from, each with its source.

Apple II slot values come from docs/HARDWARE.md "Mechanical" and the sources it
cites: [TN28] Apple IIgs Tech Note #28 (Apple IIgs Tech Notes, Developer CD,
March 1993), and [MA] the Peripheral_Bus_Edge footprint of
github.com/markadev/KiCad-AppleII at 471d84d7 (read for its numbers, not
vendored). The SO-DIMM socket values come from its manufacturer drawing [TE].
"""

# --- Apple II peripheral slot -------------------------------------------------

PITCH_MM = 2.54            # 0.1" finger pitch, 25 per side [TN28]
MAX_LEN_MM = 177.8         # 7.00": longest card for slots 1-3, where the fan is [TN28]
MAX_HEIGHT_MM = 69.85      # 2.75" above the board edge, i.e. above the tab [TN28]
# The tab is the part of the card that enters the slot. 65.8 mm (2.59") is [MA]'s
# Edge.Cuts tab (x = +-32.893). TN28 gives a 2.950" (74.9 mm) finger region
# instead; HARDWARE.md flags the disagreement: check against a real card.
TAB_WIDTH_MM = 65.8
TAB_DEPTH_MM = 7.49        # [MA]'s tab, y = -3.175 .. 4.318 (0.295"); TN28's
#                            3.05" - 2.75" would give 0.30" (7.62 mm)
TAB_REAR_OFFSET_MM = 9.525  # 0.375": the finger region ends this far from the
#                             card's rear edge [TN28]
BEVEL_KEEPOUT_MM = 1.0     # no copper this close to the tab's bottom edge.
#                            JLCPCB: the bevelled area "can not have copper",
#                            no figure given (jlcpcb.com/help/article/
#                            jlcpcb-gold-fingers); 1.0 mm is the plan's margin.
FINGER_W_MM = 1.78         # 0.070" wide, leaving 0.76 mm between fingers;
FINGER_L_MM = 6.35         # 0.250" long. The plan's values ([MA]: 1.4 x 7.112);
#                            with the keepout they end 7.35 mm up, inside the tab.

# --- DDR2 SO-DIMM 200 socket --------------------------------------------------
#
# TE Connectivity 1473005-4: DDR2 (1.8 V key) SO-DIMM socket, 200 positions,
# 0.6 mm pitch, standard (not reverse) type, 5.2 mm module height. LCSC C428482,
# 486 in stock on 2026-10-28 (JLCPCB parts API). The iCESugar-Pro is a "DDR2
# SODIMM form factor" module (github.com/wuxx/icesugar-pro README); its doc/
# folder carries a JST DM-200P sheet, a 14 mm socket, too tall for a slot.
#
# [TE] TE customer drawing C-1473005 rev J3 (15 FEB 2016), "DDR1 & DDR2 SODIMM
# SOCKET 0.6mm PITCH 200POS STANDARD PROFILE STANDARD TYPE", 4 sheets:
#   https://www.lcsc.com/datasheet/lcsc_datasheet_1912111437_TE-Connectivity-1473005-4_C428482.pdf
#   (served as https://datasheet.lcsc.com/datasheet/pdf/6b1da8624f410598280fd2881aa8acc8.pdf,
#    sha256 044e9af0bde8d1ca551723af3314136c825c756a136a06701ae0a5b41f5679b7).
# Sheet 2 is the "REFERENCE P.C. BOARD PATTERN LAYOUT (CONNECTOR MOUNTING SIDE)";
# the drawing's 1473005-1 is the DDR1 key, same pads. Values are nominal. The
# key only decides the part number: 1473005-4's sits 2.7 mm past pin 39 (sheet 1
# table, D3 for 1.8 V), DDR1's 1.8 mm; a DDR1 socket would refuse the module.
#
# Sheet 2's layout, seen from the mounting side: the two plastic bosses sit on
# one line ("the boss line"); boss 1 (1.6 mm hole) is at the pin-1 end. Odd pins
# 1..199 form the row behind the boss line, even pins 2..200 the row on the
# module side, where the solder pegs are. Each row has a 4.2 mm gap for the
# key between pins 39/41 and 40/42, near the pin-1 end.

SODIMM_LCSC = "C428482"
SODIMM_MPN = "TE 1473005-4"
SODIMM_DATASHEET = ("https://www.lcsc.com/datasheet/"
                    "lcsc_datasheet_1912111437_TE-Connectivity-1473005-4_C428482.pdf")
SODIMM_PINS = 200
SODIMM_PITCH_MM = 0.6           # sheet 2: "TYP 0.6+-0.03 PITCH", within each row
SODIMM_KEY_GAP_MM = 4.2         # sheet 2: pin 39 -> 41 and 40 -> 42, "4.2+-0.03"
SODIMM_ROW_SPAN_MM = 63.0       # sheet 2: pin 1 -> 199 and 2 -> 200, "63+-0.03"
#                                 (= 19 x 0.6 + 4.2 + 79 x 0.6)
SODIMM_BOSS_PITCH_MM = 66.8     # sheet 2: boss 1 -> boss 2, "66.8+-0.03"
SODIMM_BOSS1_TO_PIN1_MM = 1.75  # sheet 2: boss line -> pin 1, along the row, "1.75+-0.03"
# Pin 2 is offset from pin 1 toward pin 200 by what the three above leave over,
# 66.8 - 2 x 1.75 - 63 = 0.3 mm (half a pitch), so pin 200 is 1.75 mm from
# boss 2 and both rows centre on sheet 2's "33.4 REF" half-span.
SODIMM_ROW_STAGGER_MM = round(SODIMM_BOSS_PITCH_MM - 2 * SODIMM_BOSS1_TO_PIN1_MM
                              - SODIMM_ROW_SPAN_MM, 6)
SODIMM_PAD_W_MM = 0.35          # sheet 2: pad width along the row, "0.35+-0.03"
SODIMM_PAD_L_MM = 2.0           # sheet 2: pad length across the row, "2+-0.05"
SODIMM_PAD_INNER_MM = 3.1       # sheet 2: boss line -> inner pad edge, both rows, "3.1+-0.05"
SODIMM_BOSS1_DRILL_MM = 1.6     # sheet 2: boss 1 hole "phi1.6+-0.1", pin-1 end
SODIMM_BOSS2_DRILL_MM = 1.1     # sheet 2: boss 2 hole "phi1.1+-0.1", pin-200 end
SODIMM_PEG_ALONG_MM = 4.5       # sheet 2: solder-peg pad along the row, "4.5+-0.03 (2PLC)"
SODIMM_PEG_ACROSS_MM = 4.6      # sheet 2: solder-peg pad across the row, "4.6+-0.1 (2PLC)"
SODIMM_PEG_INNER_MM = 30.3      # sheet 2: centre -> inner peg-pad edge, "30.3 REF"
#                                 ("60.6+-0.03" between the two pegs)
SODIMM_PEG_OFFSET_MM = 12.0     # sheet 2: boss line -> peg-pad centre, module side, "12+-0.05"
SODIMM_HEIGHT_MM = 5.2          # sheet 1: "MODULE HEIGHT 5.2 +1/-0.2"
SODIMM_HOUSING_LEN_MM = 71.1    # sheet 1: housing length, "71.1 MAX."
SODIMM_HOUSING_HALF_MM = 4.5    # sheet 1: boss line -> each housing face, "4.5"
SODIMM_EJECTOR_W_MM = 75.0      # sheet 2: "75 (EJECTOR MAXIMUM WIDTH)", latches open
# The mated module's far edge is not dimensioned; measured off sheet 1's
# "RECOMMENDED MATING P.C.B OUTLINE" (drawn 2:1) as 30.5 mm from the housing's
# rear face, so 26.0 mm from the boss line (+-0.2 mm). Used for the courtyard only.
SODIMM_MODULE_REACH_MM = 26.0
