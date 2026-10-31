# SPDX-License-Identifier: GPL-3.0-or-later
"""hw-edge: the Apple II edge fingers, checked on the board (Review Focus 1).

    python3 hw/tools/check_edge.py BOARD.kicad_pcb              # prints HW EDGE OK
    python3 hw/tools/check_edge.py --selftest BOARD.kicad_pcb   # HW EDGE SELFTEST OK

A reversed or mirrored finger footprint puts +5 V on a signal pin, and neither ERC nor
DRC can see it: the schematic is right, and the board's own checks only compare the
board to itself. So everything here is measured on the placed board (pcbnew), not on
the footprint file or the schematic: the outline against slotgeom, J1's pads (side,
pitch, opposite pairs, pin 1 toward the front, which pins carry power) and the bevel
keepout. The board has four copper layers: no copper of In1.Cu or In2.Cu (zones, vias, through
pads) may lie below the tab's top edge, and none within EDGE_MIN_MM of the board edge, so
the bevel never exposes an inner layer. The selftest plants seven faults on a copy of the
board; each must be found.
"""
import argparse
import sys
from pathlib import Path

import pcbnew

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from hw.tools import slotgeom as g

TOL_MM = 0.05               # outline and finger geometry
EDGE_TOL_MM = 0.001         # "is this copper inside the keepout" (1 um)
EDGE_MIN_MM = 0.5           # inner copper keeps this from every board edge (build_board's)
INNER = (pcbnew.In1_Cu, pcbnew.In2_Cu)
J1 = "J1"
# What the netlist says the Apple II slot's power pins are (TN28): pin -> net.
POWER_PINS = {25: "+5V_SLOT", 26: "GND", 33: "-12V_SLOT", 34: "-5V_SLOT", 50: "+12V_SLOT"}


def _mm(v: int) -> float:
    return pcbnew.ToMM(v)


# --- outline ------------------------------------------------------------------------

def outline_vertices(board) -> list[tuple[float, float]]:
    pts = []
    for d in board.GetDrawings():
        if d.GetLayer() == pcbnew.Edge_Cuts and hasattr(d, "GetStart"):
            pts += [(_mm(d.GetStart().x), _mm(d.GetStart().y)),
                    (_mm(d.GetEnd().x), _mm(d.GetEnd().y))]
    return pts


def tab_geometry(board) -> dict | None:
    """Outline length, body height, and the tab's x range and depth, from Edge.Cuts."""
    pts = outline_vertices(board)
    if not pts:
        return None
    xs = [p[0] for p in pts]
    ys = sorted({round(p[1], 3) for p in pts})
    if len(ys) < 3:
        return None
    bottom = ys[-1]
    tab_xs = sorted({p[0] for p in pts if abs(p[1] - bottom) < 1e-3})
    if len(tab_xs) != 2:
        return None
    return {"xmin": min(xs), "xmax": max(xs), "length": max(xs) - min(xs),
            "top": ys[0], "body_bottom": ys[-2], "bottom": bottom,
            "height": ys[-2] - ys[0], "depth": bottom - ys[-2],
            "tab_l": tab_xs[0], "tab_r": tab_xs[1]}


def problems_outline(board) -> list[str]:
    t = tab_geometry(board)
    if t is None:
        return ["outline: no Edge.Cuts outline with a finger tab"]
    out = []
    if t["length"] > g.MAX_LEN_MM + 1e-6:
        out.append(f"outline length {t['length']:.2f} mm exceeds {g.MAX_LEN_MM} mm")
    if t["height"] > g.MAX_HEIGHT_MM + 1e-6:
        out.append(f"outline height above the tab {t['height']:.2f} mm exceeds "
                   f"{g.MAX_HEIGHT_MM} mm")
    for what, got, want in [("tab width", t["tab_r"] - t["tab_l"], g.TAB_WIDTH_MM),
                            ("tab depth", t["depth"], g.TAB_DEPTH_MM),
                            ("tab rear offset", t["xmax"] - t["tab_r"], g.TAB_REAR_OFFSET_MM)]:
        if abs(got - want) > TOL_MM:
            out.append(f"outline {what} {got:.3f} mm, slotgeom says {want} mm")
    return out


# --- fingers ------------------------------------------------------------------------

def problems_fingers(board) -> list[str]:
    fp = next((f for f in board.GetFootprints() if f.GetReference() == J1), None)
    if fp is None:
        return [f"no {J1} (the finger footprint) on the board"]
    t = tab_geometry(board)
    out = []
    if fp.IsFlipped():
        out.append(f"{J1} is on B.Cu: the fingers' pins 1-25 must be on F.Cu, so J1 must "
                   "sit on the front (component) side")
    if abs((fp.GetOrientationDegrees() + 180) % 360 - 180) > 0.01:
        out.append(f"{J1} is rotated {fp.GetOrientationDegrees():.1f} degrees, expected 0")
    pads = {}
    for p in fp.Pads():
        pads.setdefault(p.GetNumber(), []).append(p)
    missing = [n for n in range(1, 51) if str(n) not in pads]
    if missing or len(pads) != 50 or any(len(v) != 1 for v in pads.values()):
        return out + [f"{J1} does not have exactly pads 1-50 (missing {missing})"]
    pad = {n: pads[str(n)][0] for n in range(1, 51)}
    x = {n: _mm(pad[n].GetX()) for n in pad}
    y = {n: _mm(pad[n].GetY()) for n in pad}

    for n in range(1, 51):
        want = pcbnew.F_Cu if n <= 25 else pcbnew.B_Cu
        other = pcbnew.B_Cu if n <= 25 else pcbnew.F_Cu
        if not pad[n].IsOnLayer(want) or pad[n].IsOnLayer(other):
            out.append(f"{J1} pad {n} is on {'B' if n <= 25 else 'F'}.Cu, expected "
                       f"{'F' if n <= 25 else 'B'}.Cu")
    for n in range(1, 25):
        step = x[n + 1] - x[n]
        if abs(step - g.PITCH_MM) > TOL_MM:
            out.append(f"{J1} front pads {n}->{n + 1}: pitch {step:+.3f} mm, expected "
                       f"{g.PITCH_MM}")
    for n in range(26, 50):
        step = x[n + 1] - x[n]
        if abs(step + g.PITCH_MM) > TOL_MM:
            out.append(f"{J1} back pads {n}->{n + 1}: pitch {step:+.3f} mm, expected "
                       f"{-g.PITCH_MM}")
    for n in range(1, 26):
        if abs(x[n] - x[51 - n]) > TOL_MM:
            out.append(f"{J1} pads {n} and {51 - n} are not opposite "
                       f"(x {x[n]:.3f} vs {x[51 - n]:.3f})")
    if t is not None:
        if x[1] - t["xmin"] >= t["xmax"] - x[25]:
            out.append(f"{J1} pin 1 is not toward the front of the card "
                       f"(x {x[1]:.2f}, pin 25 at {x[25]:.2f})")
        centre = (t["tab_l"] + t["tab_r"]) / 2
        if abs(x[13] - centre) > TOL_MM:
            out.append(f"{J1} fingers are not centred in the tab (pin 13 x {x[13]:.3f}, "
                       f"tab centre {centre:.3f})")
        for n in pad:
            bb = pad[n].GetBoundingBox()
            if (_mm(bb.GetLeft()) < t["tab_l"] or _mm(bb.GetRight()) > t["tab_r"]
                    or _mm(bb.GetTop()) < t["body_bottom"] - 1e-6
                    or _mm(bb.GetBottom()) > t["bottom"] - g.BEVEL_KEEPOUT_MM + EDGE_TOL_MM):
                out.append(f"{J1} pad {n} is outside the tab's finger area")
    for n in pad:
        w, h = _mm(pad[n].GetSizeX()), _mm(pad[n].GetSizeY())
        if abs(w - g.FINGER_W_MM) > TOL_MM or abs(h - g.FINGER_L_MM) > TOL_MM:
            out.append(f"{J1} pad {n} is {w:.2f} x {h:.2f} mm, expected "
                       f"{g.FINGER_W_MM} x {g.FINGER_L_MM}")
    for n in range(2, 51):
        if abs(y[n] - y[1]) > TOL_MM:
            out.append(f"{J1} pad {n} is not level with pad 1")
    for n, net in POWER_PINS.items():
        if pad[n].GetNetname() != net:
            out.append(f"{J1} pin {n} is on net '{pad[n].GetNetname()}', expected {net}")
    return out


# --- bevel keepout ------------------------------------------------------------------

def problems_bevel(board) -> list[str]:
    t = tab_geometry(board)
    if t is None:
        return []
    limit = t["bottom"] - g.BEVEL_KEEPOUT_MM + EDGE_TOL_MM
    out = []
    for item in board.GetTracks():
        if item.Type() == pcbnew.PCB_VIA_T:
            low = _mm(item.GetY()) + _mm(item.GetWidth(pcbnew.F_Cu)) / 2
            what = "via"
        else:
            low = max(_mm(item.GetStart().y), _mm(item.GetEnd().y)) + _mm(item.GetWidth()) / 2
            what = "track"
        if low > limit:
            out.append(f"{what} at ({_mm(item.GetX()):.2f}, {_mm(item.GetY()):.2f}) is inside "
                       f"the {g.BEVEL_KEEPOUT_MM} mm bevel keepout")
    for fp in board.GetFootprints():
        for p in fp.Pads():
            if p.IsOnCopperLayer() and _mm(p.GetBoundingBox().GetBottom()) > limit:
                out.append(f"pad {fp.GetReference()}.{p.GetNumber()} is inside the "
                           f"{g.BEVEL_KEEPOUT_MM} mm bevel keepout")
    covered = set()
    for z in board.Zones():
        if z.GetIsRuleArea():
            bb = z.GetBoundingBox()
            if (z.GetDoNotAllowTracks() and z.GetDoNotAllowVias() and z.GetDoNotAllowZoneFills()
                    and _mm(bb.GetTop()) <= t["bottom"] - g.BEVEL_KEEPOUT_MM + 1e-6
                    and _mm(bb.GetBottom()) >= t["bottom"] - 1e-6
                    and _mm(bb.GetLeft()) <= t["tab_l"] and _mm(bb.GetRight()) >= t["tab_r"]):
                covered |= {L for L in (pcbnew.F_Cu, pcbnew.B_Cu) if z.IsOnLayer(L)}
            continue
        if _mm(z.GetBoundingBox().GetBottom()) > limit:
            out.append(f"zone '{z.GetZoneName()}' reaches into the bevel keepout")
    if covered != {pcbnew.F_Cu, pcbnew.B_Cu}:
        out.append("no bevel rule area over the tab's bottom band on both copper layers")
    return out


# --- inner copper -------------------------------------------------------------------

def _inner_edge_gap(t: dict, x: float, y: float) -> float:
    """How far (x, y), a point inside the body, is from the body's own edges. The tab's
    top edge is not a board edge where the tab joins the body, so inside the tab's x range
    the lower limit is the tab's top, which problems_inner checks apart."""
    gaps = [x - t["xmin"], t["xmax"] - x, y - t["top"]]
    if not (t["tab_l"] <= x <= t["tab_r"]):
        gaps.append(t["body_bottom"] - y)
    return min(gaps)


def problems_inner(board) -> list[str]:
    """No copper on In1.Cu / In2.Cu below the tab's top edge, none nearer a board edge than
    EDGE_MIN_MM: zones (every outline vertex), vias (their pad circle), through-hole pads."""
    t = tab_geometry(board)
    if t is None:
        return []
    out = []
    line = t["body_bottom"] + EDGE_TOL_MM
    for z in board.Zones():
        if z.GetIsRuleArea() or not any(z.IsOnLayer(L) for L in INNER):
            continue
        name = z.GetZoneName() or z.GetNetname()
        poly = z.Outline()
        for i in range(poly.OutlineCount()):
            for j in range(poly.COutline(i).PointCount()):
                v = poly.COutline(i).CPoint(j)
                x, y = _mm(v.x), _mm(v.y)
                if y > line:
                    out.append(f"inner copper: zone '{name}' reaches y {y:.2f} mm, below the "
                               f"tab's top edge (y {t['body_bottom']:.2f} mm)")
                    break
                if _inner_edge_gap(t, x, y) < EDGE_MIN_MM - EDGE_TOL_MM:
                    out.append(f"inner copper: zone '{name}' comes within "
                               f"{_inner_edge_gap(t, x, y):.2f} mm of the board edge at "
                               f"({x:.2f}, {y:.2f}); the edge clearance is {EDGE_MIN_MM} mm")
                    break
    for item in board.GetTracks():
        if item.Type() != pcbnew.PCB_VIA_T:
            continue
        x, y, r = _mm(item.GetX()), _mm(item.GetY()), _mm(item.GetWidth(pcbnew.F_Cu)) / 2
        if y + r > line:
            out.append(f"inner copper: via at ({x:.2f}, {y:.2f}) has its inner pad below the "
                       f"tab's top edge")
        elif _inner_edge_gap(t, x, y) - r < EDGE_MIN_MM - EDGE_TOL_MM:
            out.append(f"inner copper: via at ({x:.2f}, {y:.2f}) is within {EDGE_MIN_MM} mm of "
                       "the board edge on the inner layers")
    for fp in board.GetFootprints():
        for p in fp.Pads():
            if p.HasHole() and _mm(p.GetBoundingBox().GetBottom()) > line:
                out.append(f"inner copper: through-hole pad {fp.GetReference()}.{p.GetNumber()} "
                           "has its inner pad below the tab's top edge")
    return out


def problems(board) -> list[str]:
    return (problems_outline(board) + problems_fingers(board) + problems_bevel(board)
            + problems_inner(board))


# --- the selftest's planted faults --------------------------------------------------

def _j1(board):
    return next(f for f in board.GetFootprints() if f.GetReference() == J1)


def move_pad_13(board) -> None:
    for p in _j1(board).Pads():
        if p.GetNumber() == "13":
            p.SetX(p.GetX() + pcbnew.FromMM(0.5))


def flip_J1_to_back(board) -> None:
    j1 = _j1(board)
    j1.Flip(j1.GetPosition(), pcbnew.FLIP_DIRECTION_LEFT_RIGHT)


def stretch_to_180mm(board) -> None:
    t = tab_geometry(board)
    for d in board.GetDrawings():
        if d.GetLayer() == pcbnew.Edge_Cuts and hasattr(d, "GetStart"):
            for get, setx in [(d.GetStart, d.SetStart), (d.GetEnd, d.SetEnd)]:
                v = get()
                if abs(_mm(v.x) - t["xmax"]) < 1e-3:
                    setx(pcbnew.VECTOR2I(pcbnew.FromMM(t["xmin"] + 180.0), v.y))


def track_in_bevel(board) -> None:
    t = tab_geometry(board)
    y = pcbnew.FromMM(t["bottom"] - g.BEVEL_KEEPOUT_MM / 2)
    cx = (t["tab_l"] + t["tab_r"]) / 2
    tr = pcbnew.PCB_TRACK(board)
    tr.SetStart(pcbnew.VECTOR2I(pcbnew.FromMM(cx - 5), y))
    tr.SetEnd(pcbnew.VECTOR2I(pcbnew.FromMM(cx + 5), y))
    tr.SetWidth(pcbnew.FromMM(0.15))
    tr.SetLayer(pcbnew.F_Cu)
    board.Add(tr)


def _inner_zone(board, layer, rect, name):
    z = pcbnew.ZONE(board)
    ls = pcbnew.LSET()
    ls.AddLayer(layer)
    z.SetLayerSet(ls)
    z.SetZoneName(name)
    (x0, y0), (x1, y1) = rect
    z.Outline().NewOutline()
    for x, y in [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]:
        z.Outline().Append(pcbnew.FromMM(x), pcbnew.FromMM(y))
    board.Add(z)


def inner_copper_in_tab(board) -> None:
    """A copper pour on In2.Cu over the tab, down toward the tab's bottom edge."""
    t = tab_geometry(board)
    _inner_zone(board, pcbnew.In2_Cu, ((t["tab_l"] + 1, t["body_bottom"] - 2),
                                      (t["tab_r"] - 1, t["body_bottom"] + t["depth"] - 3)),
                "PLANT_TAB")


def inner_zone_to_edge(board) -> None:
    """An In1.Cu pour out to 0.1 mm from the front edge."""
    t = tab_geometry(board)
    _inner_zone(board, pcbnew.In1_Cu, ((t["xmin"] + 0.1, t["top"] + 5),
                                      (t["xmin"] + 10, t["top"] + 15)), "PLANT_EDGE")


def inner_via_in_tab(board) -> None:
    """A via in the tab: its inner pads are in the tab."""
    t = tab_geometry(board)
    v = pcbnew.PCB_VIA(board)
    v.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM((t["tab_l"] + t["tab_r"]) / 2),
                                  pcbnew.FromMM(t["body_bottom"] + 1.5)))
    v.SetWidth(pcbnew.FromMM(0.6))
    v.SetDrill(pcbnew.FromMM(0.3))
    board.Add(v)


MUTATIONS = {"move_pad_13": move_pad_13, "flip_J1_to_back": flip_J1_to_back,
             "stretch_to_180mm": stretch_to_180mm, "track_in_bevel": track_in_bevel,
             "inner_copper_in_tab": inner_copper_in_tab,
             "inner_zone_to_edge": inner_zone_to_edge, "inner_via_in_tab": inner_via_in_tab}
# The message each planted fault must produce (a substring of one of the problems).
EXPECT = {"move_pad_13": "pitch", "flip_J1_to_back": "J1 is on B.Cu",
          "stretch_to_180mm": "outline length", "track_in_bevel": "bevel keepout",
          "inner_copper_in_tab": "zone 'PLANT_TAB' reaches y",
          "inner_zone_to_edge": "zone 'PLANT_EDGE' comes within",
          "inner_via_in_tab": "inner pad below the tab's top edge"}


def selftest(path: Path) -> int:
    clean = problems(pcbnew.LoadBoard(str(path)))
    if clean:
        print("selftest: the board itself is not clean:\n  " + "\n  ".join(clean))
        return 1
    bad = 0
    for name, mutate in MUTATIONS.items():
        board = pcbnew.LoadBoard(str(path))
        mutate(board)
        found = problems(board)
        if not any(EXPECT[name] in p for p in found):
            print(f"selftest: planted fault {name} was not found (got {found})")
            bad += 1
    if bad:
        return 1
    print("HW EDGE SELFTEST OK")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("board", type=Path)
    ap.add_argument("--selftest", action="store_true",
                    help="plant each fault in a copy of BOARD; every one must be found")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest(a.board)
    found = problems(pcbnew.LoadBoard(str(a.board)))
    for p in found:
        print(f"HW EDGE: {p}")
    if found:
        return 1
    print("HW EDGE OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
