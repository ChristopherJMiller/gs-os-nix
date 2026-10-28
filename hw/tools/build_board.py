# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the carrier's KiCad 10 board from the netlist and a placement file.

    python3 hw/tools/build_board.py NETLIST PLACEMENT OUT.kicad_pcb [--update]

The netlist (hw/tools/netlist.py) says which footprint each part has, what its fields
are and which net each pad is on; placement.toml says where each part goes. Pad nets
come only from the netlist, never from net names written anywhere else. Nothing here
routes: a fresh build has footprints, the outline, the GND zones and the bevel rule
area, and `update=True` (used from Task 8 on) keeps tracks, vias and zones.

Board frame: the origin is the front-top corner of the card, x grows toward the card's
rear and y grows downward, so the finger tab hangs below the body at the rear (J1 is
placed from slotgeom, not from the placement file).

placement.toml:

    [board]
    length_mm = 84.0       # <= slotgeom.MAX_LEN_MM
    height_mm = 56.0       # body above the tab, <= slotgeom.MAX_HEIGHT_MM

    [parts.R1]
    x = 12.0
    y = 30.5
    rot = 90               # degrees, KiCad's sense
    side = "F"             # or "B"
"""
import argparse
import json
import os
import sys
import tomllib
from pathlib import Path

import pcbnew

if __package__ in (None, ""):          # run as a script: put the repo root on the path
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from hw.tools import slotgeom as g
from hw.tools.netlist import Netlist, load

REPO = Path(__file__).resolve().parents[2]
LOCAL_LIB = REPO / "hw" / "lib"            # <nickname>.pretty holds the generated parts
LICENSE_LINE = "SPDX-License-Identifier: CERN-OHL-S-2.0"

# JLCPCB 2-layer, no surcharge. carrier.kicad_pro carries the same numbers.
TRACK_MM = 0.15
CLEARANCE_MM = 0.15
VIA_MM = 0.6
VIA_DRILL_MM = 0.3
EDGE_CLEARANCE_MM = 0.5
GND = "GND"
FIXED = "J1"                               # the fingers: placed from slotgeom
_SKIP_FIELDS = {"Reference", "Value", "Datasheet", "Description", "Footprint"}


def _mm(v: float) -> int:
    return pcbnew.FromMM(v)


def _pt(x: float, y: float) -> "pcbnew.VECTOR2I":
    return pcbnew.VECTOR2I(_mm(x), _mm(y))


# --- geometry -----------------------------------------------------------------------

def outline(length: float, height: float) -> list[tuple[float, float]]:
    """Board outline vertices, clockwise from the front-top corner: the body, with the
    finger tab hanging below it at the rear."""
    tab_r = length - g.TAB_REAR_OFFSET_MM
    tab_l = tab_r - g.TAB_WIDTH_MM
    return [(0, 0), (length, 0), (length, height), (tab_r, height),
            (tab_r, height + g.TAB_DEPTH_MM), (tab_l, height + g.TAB_DEPTH_MM),
            (tab_l, height), (0, height)]


def j1_position(length: float, height: float) -> tuple[float, float]:
    """The finger footprint's origin: the middle of the tab's bottom edge."""
    return (length - g.TAB_REAR_OFFSET_MM - g.TAB_WIDTH_MM / 2,
            height + g.TAB_DEPTH_MM)


# --- footprints ---------------------------------------------------------------------

def _lib_dir(nick: str) -> Path:
    local = LOCAL_LIB / f"{nick}.pretty"
    if local.is_dir():
        return local
    root = os.environ.get("KICAD10_FOOTPRINT_DIR")
    if not root:
        raise SystemExit("KICAD10_FOOTPRINT_DIR is not set (use `nix develop .#hw`)")
    return Path(root) / f"{nick}.pretty"


def load_footprint(fpid: str) -> "pcbnew.FOOTPRINT":
    nick, _, name = fpid.partition(":")
    fp = pcbnew.FootprintLoad(str(_lib_dir(nick)), name)
    if fp is None:
        raise SystemExit(f"footprint {fpid} not found in {_lib_dir(nick)}")
    fp.SetFPID(pcbnew.LIB_ID(nick, name))      # record lib:name, as the netlist has it
    return fp


def _apply_netlist(board, fp, ref: str, nl: Netlist) -> None:
    """Value, fields and pad nets of `fp` from the netlist: the only source of any."""
    info = nl.parts[ref]
    fp.SetReference(ref)
    fp.SetValue(info.value)
    for name, val in info.fields.items():
        if name in _SKIP_FIELDS or not val:
            continue
        fp.SetField(name, val)
    for f in fp.GetFields():               # only the reference is drawn
        if f.GetName() != "Reference":
            f.SetVisible(False)
    pin_net = {pin: net for net, nodes in nl.nets.items() for r, pin in nodes if r == ref}
    for pad in fp.Pads():
        net = pin_net.get(pad.GetNumber())
        if net is None:                    # e.g. the SODIMM's solder pegs (ruling R9)
            pad.SetNetCode(0)
        else:
            pad.SetNet(board.FindNet(net))


# --- outline, zones ---------------------------------------------------------------

def _add_outline(board, length: float, height: float) -> None:
    for d in list(board.GetDrawings()):
        if d.GetLayer() == pcbnew.Edge_Cuts:
            board.Remove(d)
    pts = outline(length, height)
    for (x0, y0), (x1, y1) in zip(pts, pts[1:] + pts[:1]):
        seg = pcbnew.PCB_SHAPE(board)
        seg.SetShape(pcbnew.SHAPE_T_SEGMENT)
        seg.SetLayer(pcbnew.Edge_Cuts)
        seg.SetWidth(_mm(0.05))
        seg.SetStart(_pt(x0, y0))
        seg.SetEnd(_pt(x1, y1))
        board.Add(seg)


def _zone(board, layers, rect, name: str):
    z = pcbnew.ZONE(board)
    z.SetLayerSet(layers)
    z.SetZoneName(name)
    (x0, y0), (x1, y1) = rect
    z.Outline().NewOutline()
    for x, y in [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]:
        z.Outline().Append(_mm(x), _mm(y))
    board.Add(z)
    return z


def _add_zones(board, length: float, height: float) -> None:
    gnd = board.FindNet(GND)
    if gnd is None:
        raise SystemExit(f"the netlist has no {GND} net")
    for layer, name in [(pcbnew.F_Cu, "GND_F"), (pcbnew.B_Cu, "GND_B")]:
        ls = pcbnew.LSET()
        ls.AddLayer(layer)
        z = _zone(board, ls, ((0, 0), (length, height)), name)
        z.SetNet(gnd)
        z.SetMinThickness(_mm(0.2))
        z.SetLocalClearance(_mm(CLEARANCE_MM))
    # No copper where the tab's bevel is cut (the keepout above): a rule area over the
    # tab's bottom band, on both layers.
    tab_r = length - g.TAB_REAR_OFFSET_MM
    ys = height + g.TAB_DEPTH_MM
    ls = pcbnew.LSET()
    ls.AddLayer(pcbnew.F_Cu)
    ls.AddLayer(pcbnew.B_Cu)
    ra = _zone(board, ls, ((tab_r - g.TAB_WIDTH_MM - 1, ys - g.BEVEL_KEEPOUT_MM),
                           (tab_r + 1, ys + 0.5)), "BEVEL_KEEPOUT")
    ra.SetIsRuleArea(True)
    ra.SetDoNotAllowTracks(True)
    ra.SetDoNotAllowVias(True)
    ra.SetDoNotAllowZoneFills(True)
    ra.SetDoNotAllowPads(False)
    ra.SetDoNotAllowFootprints(False)


# --- the build ----------------------------------------------------------------------

def read_placement(path: Path) -> dict:
    with open(path, "rb") as f:
        return tomllib.load(f)


def build(netlist: Path, placement: Path, out: Path, update: bool) -> None:
    nl = load(netlist)
    pl = read_placement(placement)
    length = float(pl["board"]["length_mm"])
    height = float(pl["board"]["height_mm"])
    if length > g.MAX_LEN_MM:
        raise SystemExit(f"board length {length} mm exceeds {g.MAX_LEN_MM} mm")
    if height > g.MAX_HEIGHT_MM:
        raise SystemExit(f"board height {height} mm exceeds {g.MAX_HEIGHT_MM} mm")
    parts = pl.get("parts", {})
    if FIXED in parts:
        raise SystemExit(f"{FIXED} is placed from slotgeom; remove it from {placement}")
    unplaced = sorted(r for r in nl.parts if r != FIXED and r not in parts)
    if unplaced:
        raise SystemExit(f"parts with no placement: {', '.join(unplaced)}")
    unknown = sorted(r for r in parts if r not in nl.parts)
    if unknown:
        raise SystemExit(f"placements for parts not in the netlist: {', '.join(unknown)}")

    out = Path(out)
    fresh = not (update and out.exists())
    board = pcbnew.BOARD() if fresh else pcbnew.LoadBoard(str(out))

    for name in nl.nets:
        if name and board.FindNet(name) is None:
            board.Add(pcbnew.NETINFO_ITEM(board, name))

    have = {fp.GetReference(): fp for fp in board.GetFootprints()}
    for ref, fp in have.items():
        if ref not in nl.parts:
            board.Remove(fp)
    for ref, info in nl.parts.items():
        old = have.get(ref)
        if old is not None and old.GetFPIDAsString() == info.footprint:
            _apply_netlist(board, old, ref, nl)       # keeps where it is
            continue
        fp = load_footprint(info.footprint)
        board.Add(fp)
        if old is not None:                           # footprint changed: same place
            pos, rot, back = old.GetPosition(), old.GetOrientationDegrees(), old.IsFlipped()
            board.Remove(old)
        else:
            if ref == FIXED:
                x, y = j1_position(length, height)
                rot, back = 0.0, False
            else:
                p = parts[ref]
                x, y = float(p["x"]), float(p["y"])
                rot, back = float(p.get("rot", 0)), p.get("side", "F") == "B"
            pos = _pt(x, y)
        fp.SetPosition(pos)
        if back:
            fp.Flip(pos, pcbnew.FLIP_DIRECTION_LEFT_RIGHT)
        fp.SetOrientationDegrees(rot)
        _apply_netlist(board, fp, ref, nl)

    _add_outline(board, length, height)
    if not list(board.Zones()):
        _add_zones(board, length, height)

    ds = board.GetDesignSettings()
    ds.m_MinClearance = _mm(CLEARANCE_MM)
    ds.m_CopperEdgeClearance = _mm(EDGE_CLEARANCE_MM)
    ds.SetCustomTrackWidth(_mm(TRACK_MM))
    ds.SetCustomViaSize(_mm(VIA_MM))
    ds.SetCustomViaDrill(_mm(VIA_DRILL_MM))
    tb = board.GetTitleBlock()
    tb.SetTitle("gs-os-nix Apple II carrier")
    tb.SetComment(0, LICENSE_LINE)
    board.SetFileName(str(out))
    out.parent.mkdir(parents=True, exist_ok=True)
    pcbnew.SaveBoard(str(out), board)


def project_json() -> dict:
    """carrier.kicad_pro's design rules (JLCPCB 2-layer, no surcharge)."""
    return {
        "meta": {"filename": "carrier.kicad_pro", "version": 3},
        "board": {"design_settings": {"rules": {
            "min_clearance": CLEARANCE_MM, "min_track_width": TRACK_MM,
            "min_via_diameter": VIA_MM, "min_through_hole_diameter": VIA_DRILL_MM,
            "min_copper_edge_clearance": EDGE_CLEARANCE_MM}}},
        "net_settings": {"classes": [{
            "name": "Default", "clearance": CLEARANCE_MM, "track_width": TRACK_MM,
            "via_diameter": VIA_MM, "via_drill": VIA_DRILL_MM}], "meta": {"version": 3}},
        "text_variables": {"LICENSE": "CERN-OHL-S-2.0"},
    }


def main(argv: list[str] | None = None) -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("netlist", type=Path)
    ap.add_argument("placement", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--update", action="store_true",
                    help="keep tracks, vias and zones of an existing board")
    ap.add_argument("--project", type=Path, help="also write the .kicad_pro here")
    a = ap.parse_args(argv)
    build(a.netlist, a.placement, a.out, a.update)
    if a.project:
        a.project.write_text(json.dumps(project_json(), indent=2) + "\n")
    print(f"wrote {a.out}")


if __name__ == "__main__":
    main()
