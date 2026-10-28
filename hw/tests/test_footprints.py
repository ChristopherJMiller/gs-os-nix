# SPDX-License-Identifier: GPL-3.0-or-later
"""The generated footprints, read back through KiCad 10's own parser.

A reversed or mirrored land pattern passes ERC and DRC, so the orientation is
pinned here against the sources slotgeom cites (Review Focus 1)."""
from dataclasses import dataclass
from pathlib import Path

import pytest

from hw.tools import footprints
from hw.tools import slotgeom as g

COMMITTED = Path(__file__).resolve().parents[1] / "lib" / "gsnix.pretty"
TOL = 0.01
LAYERS = ["F.Cu", "B.Cu", "F.Mask", "B.Mask", "F.Paste", "B.Paste"]


@dataclass
class Pad:
    num: str
    x: float          # centre, mm, footprint frame (KiCad: y grows downward)
    y: float
    x0: float         # copper extents
    x1: float
    y0: float
    y1: float
    smd: bool
    npth: bool
    drill: float
    layers: frozenset


def _pads(lib: Path, name: str) -> list[Pad]:
    import pcbnew
    fp = pcbnew.FootprintLoad(str(lib), name)
    assert fp is not None, f"KiCad cannot load {name}"
    assert fp.GetFPID().GetLibItemName() == name
    mm = pcbnew.ToMM
    out = []
    for p in fp.Pads():
        bb = p.GetBoundingBox()
        out.append(Pad(
            p.GetNumber(), mm(p.GetPosition().x), mm(p.GetPosition().y),
            mm(bb.GetLeft()), mm(bb.GetRight()), mm(bb.GetTop()), mm(bb.GetBottom()),
            p.GetAttribute() == pcbnew.PAD_ATTRIB_SMD,
            p.GetAttribute() == pcbnew.PAD_ATTRIB_NPTH,
            mm(p.GetDrillSize().x),
            frozenset(n for n in LAYERS if p.IsOnLayer(getattr(pcbnew, n.replace(".", "_"))))))
    return out


@pytest.fixture(scope="module")
def lib(tmp_path_factory):
    d = tmp_path_factory.mktemp("fp") / "gsnix.pretty"
    footprints.write_library(d)
    return d


@pytest.fixture(scope="module")
def edge(lib):
    lib_name, name = footprints.EDGE_ID.split(":")
    assert lib_name == "gsnix"
    pads = _pads(lib, name)
    by_num = {p.num: p for p in pads}
    assert len(by_num) == len(pads), "duplicate finger numbers"
    return by_num


@pytest.fixture(scope="module")
def sodimm(lib):
    lib_name, name = footprints.SODIMM_ID.split(":")
    assert lib_name == "gsnix"
    return _pads(lib, name)


# --- Apple II slot fingers ----------------------------------------------------

def test_edge_pad_count_and_layers(edge):
    assert sorted(edge, key=int) == [str(n) for n in range(1, 51)]
    for n, p in edge.items():
        assert p.smd, n
        want = {"F.Cu", "F.Mask"} if int(n) <= 25 else {"B.Cu", "B.Mask"}
        assert p.layers == want, (n, sorted(p.layers))   # one side only, no paste


def test_edge_pitch(edge):
    # Component side runs 1 -> 25 toward the rear (+x); solder side 26 -> 50
    # runs back toward the front, so its signed step is negative.
    for side, step in ((range(1, 26), g.PITCH_MM), (range(26, 51), -g.PITCH_MM)):
        for n in list(side)[:-1]:
            d = edge[str(n + 1)].x - edge[str(n)].x
            assert abs(d - step) <= TOL, (n, d)


def test_edge_opposites(edge):
    # Pin n faces pin 51 - n through the board: 25 (+5 V) / 26 (GND), 1 / 50.
    for n in range(1, 26):
        a, b = edge[str(n)], edge[str(51 - n)]
        assert abs(a.x - b.x) <= TOL and abs(a.y - b.y) <= TOL, n


def test_edge_pin1_front(edge):
    # x grows toward the card's rear; pin 1 (/IOSEL) is toward the front.
    assert edge["1"].x < edge["25"].x
    assert edge["50"].x < edge["26"].x


def test_edge_fingers_clear_the_bevel_and_fit_the_tab(edge):
    # The tab's bottom edge is y = 0 and the fingers run up into the board (-y).
    for n, p in edge.items():
        assert abs((p.x1 - p.x0) - g.FINGER_W_MM) <= TOL, n
        assert abs((p.y1 - p.y0) - g.FINGER_L_MM) <= TOL, n
        assert p.y1 <= -g.BEVEL_KEEPOUT_MM + TOL, n
        assert p.y0 >= -g.TAB_DEPTH_MM - TOL, n
        assert -g.TAB_WIDTH_MM / 2 <= p.x0 and p.x1 <= g.TAB_WIDTH_MM / 2, n


# --- DDR2 SO-DIMM socket ------------------------------------------------------

def test_sodimm_pads(sodimm):
    sig = {p.num: p for p in sodimm if p.num.isdigit()}
    assert sorted(sig, key=int) == [str(n) for n in range(1, 201)]
    assert sum(p.num.isdigit() for p in sodimm) == 200
    for n, p in sig.items():
        assert p.smd and p.layers == {"F.Cu", "F.Mask", "F.Paste"}, n
    odd = {round(sig[str(n)].y, 3) for n in range(1, 201, 2)}
    even = {round(sig[str(n)].y, 3) for n in range(2, 201, 2)}
    assert len(odd) == 1 and len(even) == 1 and odd != even
    for first in (1, 2):
        for n in range(first, 199, 2):
            step = sig[str(n + 2)].x - sig[str(n)].x
            want = g.SODIMM_KEY_GAP_MM if n in (39, 40) else g.SODIMM_PITCH_MM
            assert abs(step - want) <= TOL, (n, step)
    # Pin 1 is at boss 1, the 1.6 mm hole, 1.75 mm from it (TE C-1473005 sheet 2);
    # pin 200 sits the same distance from boss 2, the 1.1 mm hole.
    small, big = sorted((p for p in sodimm if p.npth), key=lambda p: p.drill)
    assert abs(big.drill - g.SODIMM_BOSS1_DRILL_MM) <= TOL
    assert abs(small.drill - g.SODIMM_BOSS2_DRILL_MM) <= TOL
    assert abs((sig["1"].x - big.x) - g.SODIMM_BOSS1_TO_PIN1_MM) <= TOL
    assert abs((small.x - sig["200"].x) - g.SODIMM_BOSS1_TO_PIN1_MM) <= TOL
    assert abs((sig["199"].x - sig["1"].x) - g.SODIMM_ROW_SPAN_MM) <= TOL


def test_sodimm_not_mirrored(sodimm):
    # Sheet 2 (connector mounting side): the even row is on the module side,
    # with the solder pegs; the odd row is behind the boss line; pin 2 is
    # staggered half a pitch from pin 1 toward pin 200.
    sig = {p.num: p for p in sodimm if p.num.isdigit()}
    pegs = [p for p in sodimm if p.num == "MP"]
    boss_y = {round(p.y, 3) for p in sodimm if p.npth}
    assert len(pegs) == 2 and len(boss_y) == 1
    y0 = boss_y.pop()
    assert all(p.smd and "F.Cu" in p.layers for p in pegs)
    assert all((p.y - y0) * (sig["2"].y - y0) > 0 for p in pegs)
    assert (sig["1"].y - y0) * (sig["2"].y - y0) < 0
    assert abs((sig["2"].x - sig["1"].x) - g.SODIMM_PITCH_MM / 2) <= TOL


def test_library_is_regenerated(tmp_path):
    written = footprints.write_library(tmp_path / "gsnix.pretty")
    assert sorted(p.name for p in written) == sorted(p.name for p in COMMITTED.iterdir())
    for p in written:
        assert p.read_bytes() == (COMMITTED / p.name).read_bytes(), p.name
