# SPDX-License-Identifier: GPL-3.0-or-later
"""silk.place_references: the reference designators on the silkscreen, placed by rule.

On a temporary copy of the committed board with the art applied (artwork.apply places the
references after the art): KiCad's DRC finds no silkscreen on a pad, on other silkscreen or over
the board edge; every reference shown sits beside its own part, nearer to it than to any other;
the hidden ones are listed, few, and still on the fab layer; and the result does not depend on
where the references were before. On small boards: a part with room gets its reference beside
it, and a part boxed in by others gets it hidden and listed."""
import shutil
from pathlib import Path

import pcbnew
import pytest

from hw.tests.test_finish import make_board, v
from hw.tools import artwork, build_board, check_drc, silk

CARRIER = Path(__file__).resolve().parents[1] / "carrier"
BOARD = CARRIER / "carrier.kicad_pcb"
ART = CARRIER / "artwork.toml"
MM = pcbnew.ToMM


def _copy(into: Path) -> Path:
    for suffix in (".kicad_pcb", ".kicad_pro", ".kicad_dru"):
        shutil.copy(BOARD.with_suffix(suffix), into / f"carrier{suffix}")
    return into / "carrier.kicad_pcb"


def _shown(board) -> list:
    return [fp for fp in board.GetFootprints()
            if fp.Reference().GetLayer() == pcbnew.F_SilkS and fp.Reference().IsVisible()]


def _box(field) -> tuple:
    bb = field.GetEffectiveShape(pcbnew.F_SilkS).BBox(0)
    return MM(bb.GetLeft()), MM(bb.GetTop()), MM(bb.GetRight()), MM(bb.GetBottom())


def _state(board) -> list:
    return sorted((fp.GetReference(), fp.Reference().IsVisible(), fp.Reference().GetX(),
                   fp.Reference().GetY(), fp.Reference().GetTextAngleDegrees())
                  for fp in board.GetFootprints())


@pytest.fixture(scope="module")
def placed(tmp_path_factory):
    path = _copy(tmp_path_factory.mktemp("silk"))
    board = pcbnew.LoadBoard(str(path))
    hidden = artwork.apply(board, ART)
    board.Save(str(path))
    return path, board, hidden


def test_kicad_finds_no_silkscreen_fault(placed):
    path, _, _ = placed
    rep = check_drc.run_drc(path, all_severities=True)
    silk_faults = [f"{v['type']}: {v['description']}" for v in rep["violations"]
                   if v["type"].startswith("silk_")]
    assert silk_faults == [], silk_faults[:10]


def test_each_reference_sits_beside_its_own_part(placed):
    _, board, _ = placed
    anchors = {fp.GetReference(): silk.anchor(fp) for fp in board.GetFootprints()}
    far, nearer_other = [], []
    for fp in _shown(board):
        ref = fp.GetReference()
        x0, y0, x1, y1 = _box(fp.Reference())
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
        a = anchors[ref]
        gap = max(a[0] - x1, x0 - a[2], a[1] - y1, y0 - a[3])     # box to box, along x or y
        if gap > silk.MAX_GAP_MM + 0.01:
            far.append((ref, round(gap, 2)))
        own = silk.distance(a, cx, cy)
        others = [r for r, o in anchors.items() if r != ref and silk.distance(o, cx, cy) <= own]
        if others:
            nearer_other.append((ref, others))
    assert not far, far
    assert not nearer_other, nearer_other


def test_no_reference_prints_under_a_part(placed):
    """KiCad does not flag silkscreen under a part's body, but the part would hide it."""
    _, board, _ = placed
    bodies = []
    for fp in board.GetFootprints():
        fab = [g.GetBoundingBox() for g in fp.GraphicalItems()
               if g.GetLayer() == pcbnew.F_Fab and g.GetClass() == "PCB_SHAPE"]
        if fab:
            bodies.append((fp.GetReference(), (min(MM(b.GetLeft()) for b in fab),
                                               min(MM(b.GetTop()) for b in fab),
                                               max(MM(b.GetRight()) for b in fab),
                                               max(MM(b.GetBottom()) for b in fab))))
    under = []
    for fp in _shown(board):
        x0, y0, x1, y1 = _box(fp.Reference())
        under += [(fp.GetReference(), r) for r, b in bodies
                  if x0 < b[2] and b[0] < x1 and y0 < b[3] and b[1] < y1]
    assert not under, under


def test_shown_references_are_upright_and_of_one_size(placed):
    _, board, _ = placed
    shown = _shown(board)
    assert len(shown) > 50
    for fp in shown:
        f = fp.Reference()
        assert f.GetTextAngleDegrees() in (0.0, 90.0), fp.GetReference()
        assert MM(f.GetTextHeight()) == pytest.approx(silk.TEXT_MM)
        assert MM(f.GetTextThickness()) == pytest.approx(silk.THICKNESS_MM)


def test_hidden_references_are_listed_few_and_still_on_the_fab_layer(placed):
    _, board, hidden = placed
    invisible = sorted(fp.GetReference() for fp in board.GetFootprints()
                       if fp.Reference().GetLayer() == pcbnew.F_SilkS
                       and not fp.Reference().IsVisible())
    assert sorted(hidden) == invisible
    assert len(hidden) <= 10, hidden
    for ref in hidden:
        fp = board.FindFootprintByReference(ref)
        fab = [t for t in fp.GraphicalItems() if t.GetClass() == "PCB_TEXT"
               and t.GetLayer() == pcbnew.F_Fab and t.GetText() == "${REFERENCE}"]
        assert fab, f"{ref} has no reference on the fab layer"


def test_the_same_wherever_the_references_were(tmp_path):
    board = pcbnew.LoadBoard(str(_copy(tmp_path)))
    artwork.apply(board, ART)
    once = _state(board)
    for fp in board.GetFootprints():
        f = fp.Reference()
        if f.GetLayer() != pcbnew.F_SilkS:       # J1's is on the fab layer, and not silk's
            continue
        f.SetPosition(f.GetPosition() + v(3, -2))
        f.SetTextAngleDegrees(45)
        f.SetVisible(False)
    silk.place_references(board)
    assert _state(board) == once


# --- small boards ----------------------------------------------------------------------------

def _part(b, ref, x, y, rot=0):
    fp = build_board.load_footprint("Resistor_SMD:R_0603_1608Metric")
    fp.SetReference(ref)
    fp.SetPosition(v(x, y))
    fp.SetOrientationDegrees(rot)
    b.Add(fp)
    return fp


def test_a_part_with_room_gets_its_reference_beside_it():
    b = make_board(None, {})
    fp = _part(b, "R1", 12, 6)
    assert silk.place_references(b) == []
    f = fp.Reference()
    assert f.IsVisible() and f.GetTextAngleDegrees() == 0
    x0, y0, x1, y1 = _box(f)
    a = silk.anchor(fp)
    assert y1 <= a[1] - silk.PAD_GAP_MM + 1e-6, "above the part, clear of its pads"
    # the 0603's own outline line runs 0.33 mm above its pads, and the text clears it by
    # SILK_GAP_MM: the first place that does, and no farther
    assert a[1] - y1 <= 0.33 + silk.SILK_GAP_MM + silk.STEP_MM, "and close to it"
    assert (x0 + x1) / 2 == pytest.approx(12, abs=0.01), "centred on it"


def test_a_reference_keeps_off_a_neighbours_body():
    """U1's body (its F.Fab outline, no silkscreen) ends 0.06 mm from where R1's reference
    would go, above R1: the reference goes below instead, BODY_GAP_MM clear. (Its centre would
    still be nearer to R1 than to U1, so only the body rule moves it. A reference that reached
    further into a body would be nearer to that part, and the nearest-part rule stops it.)"""
    b = make_board(None, {})
    r1 = _part(b, "R1", 12, 6)
    u1 = pcbnew.FOOTPRINT(b)
    u1.SetReference("U1")
    u1.Reference().SetLayer(pcbnew.F_Fab)
    body = pcbnew.PCB_SHAPE(u1, pcbnew.SHAPE_T_RECTANGLE)
    body.SetLayer(pcbnew.F_Fab)
    body.SetStart(v(12.95, 2.0))
    body.SetEnd(v(20, 5.0))
    body.SetWidth(pcbnew.FromMM(0.1))          # its outer edge: x 12.9, y 5.05
    u1.Add(body)
    pad = pcbnew.PAD(u1)
    pad.SetNumber("1")
    pad.SetShape(pcbnew.PAD_SHAPE_RECT)
    pad.SetAttribute(pcbnew.PAD_ATTRIB_SMD)
    pad.SetLayerSet(pad.SMDMask())
    pad.SetSize(v(0.8, 0.8))
    pad.SetPosition(v(19, 3))
    u1.Add(pad)
    b.Add(u1)
    assert silk.place_references(b) == []
    x0, y0, x1, y1 = _box(r1.Reference())
    g = silk.BODY_GAP_MM - 1e-6
    assert x1 <= 12.9 - g or y0 >= 5.05 + g, (x0, y0, x1, y1)


def test_a_reference_keeps_off_the_art():
    """Board silkscreen (the art) where R1's reference would go, above R1: it goes elsewhere."""
    b = make_board(None, {})
    r1 = _part(b, "R1", 12, 6)
    t = pcbnew.PCB_TEXT(b)
    t.SetLayer(pcbnew.F_SilkS)
    t.SetText("ART")
    t.SetTextSize(v(1.0, 1.0))
    t.SetTextThickness(pcbnew.FromMM(0.15))
    t.SetPosition(v(12, 4.4))
    b.Add(t)
    assert silk.place_references(b) == []
    x0, y0, x1, y1 = _box(r1.Reference())
    a0, b0, a1, b1 = _box(t)
    g = silk.SILK_GAP_MM - 1e-6
    assert x1 <= a0 - g or x0 >= a1 + g or y1 <= b0 - g or y0 >= b1 + g, ((x0, y0, x1, y1),
                                                                          (a0, b0, a1, b1))


def test_a_boxed_in_part_is_hidden_and_listed():
    """A 3 x 3 block of 0603s, 3.2 mm apart along x and 1.7 mm down: no room for the middle
    one's reference anywhere nearer to it than to a neighbour."""
    b = make_board(None, {})
    k = 0
    for j in range(3):
        for i in range(3):
            k += 1
            _part(b, f"R{k}", 8.8 + 3.2 * i, 4.3 + 1.7 * j)
    hidden = silk.place_references(b)
    assert "R5" in hidden
    assert not b.FindFootprintByReference("R5").Reference().IsVisible()
