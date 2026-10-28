# SPDX-License-Identifier: GPL-3.0-or-later
"""build_board: placement, pad nets and fields from the netlist, update mode; and
check_edge, the on-board orientation gate (Review Focus 1)."""
import math
import subprocess
import tomllib
from pathlib import Path

import pcbnew
import pytest

from hw.tools import build_board as bb
from hw.tools import check_edge as ce
from hw.tools import netlist as nlmod
from hw.tools import slotgeom as g

ROOT = Path(__file__).resolve().parents[2]
FIX = Path(__file__).parent / "fixtures"
CARRIER_BOARD = ROOT / "hw" / "carrier" / "carrier.kicad_pcb"
PLACEMENT = ROOT / "hw" / "carrier" / "placement.toml"
DECOUPLE_MM = 2.0


def mm(v):
    return pcbnew.ToMM(v)


@pytest.fixture()
def tiny(tmp_path):
    out = tmp_path / "tiny.kicad_pcb"
    bb.build(FIX / "tiny.net", FIX / "tiny_placement.toml", out, update=False)
    return out


def fp_of(board, ref):
    return next(f for f in board.GetFootprints() if f.GetReference() == ref)


def test_tiny_board(tiny):
    board = pcbnew.LoadBoard(str(tiny))
    r1, r2 = fp_of(board, "R1"), fp_of(board, "R2")
    assert (mm(r1.GetX()), mm(r1.GetY())) == pytest.approx((12.0, 30.5), abs=1e-3)
    assert r1.GetOrientationDegrees() == pytest.approx(90)
    assert not r1.IsFlipped()
    assert (mm(r2.GetX()), mm(r2.GetY())) == pytest.approx((40.25, 12.0), abs=1e-3)
    assert r2.IsFlipped()
    nets = {(f.GetReference(), p.GetNumber()): p.GetNetname()
            for f in (r1, r2) for p in f.Pads()}
    assert nets == {("R1", "1"): "N1", ("R1", "2"): "GND",
                    ("R2", "1"): "N1", ("R2", "2"): "GND"}
    assert r1.GetFieldText("LCSC") == "C21190"
    assert r1.GetValue() == "1k"
    assert not r2.HasField("LCSC")                  # tiny.net gives R2 none


def test_outline_and_zones(tiny):
    board = pcbnew.LoadBoard(str(tiny))
    assert ce.problems_outline(board) == []
    zones = {z.GetZoneName(): z for z in board.Zones()}
    assert {"GND_F", "GND_B", "BEVEL_KEEPOUT"} <= set(zones)
    assert zones["GND_F"].GetNetname() == "GND" and zones["GND_F"].IsOnLayer(pcbnew.F_Cu)
    assert zones["GND_B"].GetNetname() == "GND" and zones["GND_B"].IsOnLayer(pcbnew.B_Cu)
    ra = zones["BEVEL_KEEPOUT"]
    assert ra.GetIsRuleArea() and ra.GetDoNotAllowTracks() and ra.GetDoNotAllowVias()
    assert "CERN-OHL-S-2.0" in tiny.read_text()


def test_unplaced_part_is_error(tmp_path):
    pl = tmp_path / "p.toml"
    pl.write_text('[board]\nlength_mm = 100.0\nheight_mm = 50.0\n'
                  '[parts.R1]\nx = 1.0\ny = 2.0\n')
    with pytest.raises(SystemExit, match="R2"):
        bb.build(FIX / "tiny.net", pl, tmp_path / "o.kicad_pcb", update=False)


def test_oversize_board_is_error(tmp_path):
    pl = tmp_path / "p.toml"
    pl.write_text(FIX.joinpath("tiny_placement.toml").read_text().replace("100.0", "180.0"))
    with pytest.raises(SystemExit, match="exceeds"):
        bb.build(FIX / "tiny.net", pl, tmp_path / "o.kicad_pcb", update=False)


def test_update_keeps_tracks(tiny):
    board = pcbnew.LoadBoard(str(tiny))
    t = pcbnew.PCB_TRACK(board)
    t.SetStart(pcbnew.VECTOR2I(bb._mm(10), bb._mm(10)))
    t.SetEnd(pcbnew.VECTOR2I(bb._mm(20), bb._mm(10)))
    t.SetWidth(bb._mm(0.15))
    t.SetLayer(pcbnew.F_Cu)
    t.SetNet(board.FindNet("N1"))
    board.Add(t)
    pcbnew.SaveBoard(str(tiny), board)
    nzones = len(list(board.Zones()))
    bb.build(FIX / "tiny.net", FIX / "tiny_placement.toml", tiny, update=True)
    after = pcbnew.LoadBoard(str(tiny))
    assert len(list(after.GetTracks())) == 1
    assert len(list(after.Zones())) == nzones
    assert len(list(after.GetFootprints())) == 2


def test_update_leaves_unchanged_parts_untouched(tiny):
    """A locked footprint with a hand-moved reference survives update=True, and the
    board records the full lib:name the netlist uses."""
    board = pcbnew.LoadBoard(str(tiny))
    r1 = fp_of(board, "R1")
    assert r1.GetFPIDAsString() == "Resistor_SMD:R_0603_1608Metric"
    r1.SetLocked(True)
    ref_pos = pcbnew.VECTOR2I(bb._mm(13.3), bb._mm(27.7))
    r1.Reference().SetPosition(ref_pos)
    pcbnew.SaveBoard(str(tiny), board)
    bb.build(FIX / "tiny.net", FIX / "tiny_placement.toml", tiny, update=True)
    r1 = fp_of(pcbnew.LoadBoard(str(tiny)), "R1")
    assert r1.IsLocked()
    assert r1.Reference().GetPosition() == ref_pos


def test_update_adds_and_removes_parts(tiny, tmp_path):
    net = tmp_path / "t.net"
    net.write_text((FIX / "tiny.net").read_text().replace('"R2"', '"R3"'))
    pl = tmp_path / "p.toml"
    pl.write_text(FIX.joinpath("tiny_placement.toml").read_text().replace("R2", "R3")
                  .replace("x = 40.25", "x = 55.5"))
    bb.build(net, pl, tiny, update=True)
    board = pcbnew.LoadBoard(str(tiny))
    assert sorted(f.GetReference() for f in board.GetFootprints()) == ["R1", "R3"]
    assert mm(fp_of(board, "R3").GetX()) == pytest.approx(55.5, abs=1e-3)


# --- check_edge ---------------------------------------------------------------------

@pytest.fixture()
def carrier_board():
    return pcbnew.LoadBoard(str(CARRIER_BOARD))


def test_carrier_board_is_clean(carrier_board):
    assert ce.problems(carrier_board) == []


@pytest.mark.parametrize("name", ["move_pad_13", "flip_J1_to_back", "stretch_to_180mm",
                                  "track_in_bevel"])
def test_edge_mutation_fails(name, carrier_board):
    assert name in ce.MUTATIONS
    ce.MUTATIONS[name](carrier_board)
    found = ce.problems(carrier_board)
    assert found, f"{name} passed check_edge"
    assert any(ce.EXPECT[name] in p for p in found), found


def test_edge_catches_mirrored_fingers(carrier_board):
    """Fingers mirrored end for end (pin 1 at the rear): ERC and DRC would pass this."""
    j1 = fp_of(carrier_board, "J1")
    j1.Flip(j1.GetPosition(), pcbnew.FLIP_DIRECTION_TOP_BOTTOM)
    assert ce.problems(carrier_board)


# --- the carrier floorplan ----------------------------------------------------------

@pytest.fixture(scope="module")
def carrier_nl(tmp_path_factory):
    out = tmp_path_factory.mktemp("carrier")
    subprocess.run(["python3", "-m", "hw.carrier.top", "--out", str(out), "--no-schematic"],
                   cwd=ROOT, check=True, capture_output=True)
    return nlmod.load(out / "carrier.net")


def test_board_matches_netlist(carrier_board, carrier_nl):
    """Every part is on the board with the netlist's pad nets and fields (R8, R9)."""
    refs = {f.GetReference() for f in carrier_board.GetFootprints()}
    assert refs == set(carrier_nl.parts)
    pin_net = {}
    for net, nodes in carrier_nl.nets.items():
        for ref, pin in nodes:
            pin_net[(ref, pin)] = net
    for f in carrier_board.GetFootprints():
        ref = f.GetReference()
        for pad in f.Pads():
            assert pad.GetNetname() == pin_net.get((ref, pad.GetNumber()), ""), (ref, pad.GetNumber())
        for k, v in carrier_nl.parts[ref].fields.items():
            if v and k not in bb._SKIP_FIELDS:
                assert f.GetFieldText(k) == v, (ref, k)
    assert fp_of(carrier_board, "J1").GetFieldText("Fab") == "pcb"
    assert not fp_of(carrier_board, "J1").HasField("LCSC")


def test_every_part_is_on_the_board(carrier_board):
    box = carrier_board.GetBoardEdgesBoundingBox()
    for f in carrier_board.GetFootprints():
        if f.GetReference() != "J1":
            assert box.Contains(f.GetBoundingBox(False)), f.GetReference()


def test_placement_toml_is_consistent():
    pl = tomllib.loads(PLACEMENT.read_text())
    assert pl["board"]["length_mm"] <= g.MAX_LEN_MM
    assert pl["board"]["height_mm"] <= g.MAX_HEIGHT_MM
    assert "J1" not in pl["parts"]
    assert PLACEMENT.read_text().startswith("# SPDX-License-Identifier: CERN-OHL-S-2.0")


def _pad(fp, num):
    return next(p for p in fp.Pads() if p.GetNumber() == num)


def test_decoupling_caps_within_2mm(carrier_board):
    """Each [decouple] entry (cap = "U.pin", or a list of pins for a shared cap) has the
    cap's supply pad within 2 mm (pad edge to pad edge) of every pin it serves."""
    dec = tomllib.loads(PLACEMENT.read_text())["decouple"]
    assert len(dec) == 18
    for cap, pins in dec.items():
        c = fp_of(carrier_board, cap)
        supply = [p for p in c.Pads() if p.GetNetname() != "GND"]
        assert len(supply) == 1, cap
        for pin in ([pins] if isinstance(pins, str) else pins):
            ref, num = pin.split(".")
            ip = _pad(fp_of(carrier_board, ref), num)
            assert ip.GetNetname() == supply[0].GetNetname(), (cap, pin)
            a, b = supply[0].GetBoundingBox(), ip.GetBoundingBox()
            dx = max(0, mm(a.GetLeft() - b.GetRight()), mm(b.GetLeft() - a.GetRight()))
            dy = max(0, mm(a.GetTop() - b.GetBottom()), mm(b.GetTop() - a.GetBottom()))
            assert math.hypot(dx, dy) <= DECOUPLE_MM, (cap, pin, math.hypot(dx, dy))


def test_every_ic_supply_pin_has_a_cap(carrier_nl):
    """The [decouple] table covers each IC's rail pins (U1's VCCB pair share one cap)."""
    covered = set()
    for pins in tomllib.loads(PLACEMENT.read_text())["decouple"].values():
        covered |= set([pins] if isinstance(pins, str) else pins)
    for net in ("+3V3", "+5V"):
        for ref, pin in carrier_nl.nets[net]:
            if ref.startswith("U"):
                assert f"{ref}.{pin}" in covered, (net, ref, pin)


def test_tall_parts_are_clear_of_the_module(carrier_board):
    """The module lies flat over the board: no through-hole part inside J2's courtyard."""
    area = fp_of(carrier_board, "J2").GetCourtyard(pcbnew.F_CrtYd).BBox()
    for f in carrier_board.GetFootprints():
        if f.GetReference() == "J2":
            continue
        if any(p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH for p in f.Pads()):
            assert not area.Intersects(f.GetBoundingBox(False)), f.GetReference()
