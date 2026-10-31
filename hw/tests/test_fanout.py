# SPDX-License-Identifier: GPL-3.0-or-later
"""P19: every SMD pad on GND or +3V3 reaches its inner plane by a short neck and a tented,
locked via within 1 mm of the pad edge; none in the finger tab."""
from pathlib import Path

import pcbnew

from hw.tests.test_finish import F, make_board, v
from hw.tools import check_edge
from hw.tools import route

ROOT = Path(__file__).resolve().parents[2]
PLANES = ("GND", "+3V3")


def _vias(b, net):
    return [t for t in b.GetTracks() if t.GetClass() == "PCB_VIA" and t.GetNetname() == net]


def _pad_dist(pad, pt, layer=F) -> float:
    """Distance (mm) from `pt` (VECTOR2I) to the pad's copper outline."""
    poly = pcbnew.SHAPE_POLY_SET()
    pad.TransformShapeToPolygon(poly, layer, 0, 10000, pcbnew.ERROR_INSIDE)
    best = 1e9
    for k in range(poly.OutlineCount()):
        o = poly.Outline(k)
        n = o.PointCount()
        for i in range(n):
            best = min(best, route._seg_dist(pt, pt, o.CPoint(i), o.CPoint((i + 1) % n)))
    return best


def _pads(b, nets):
    return [(fp, p) for fp in b.GetFootprints() for p in fp.Pads() if p.GetNetname() in nets]


def _stub(tmp_path, extra=None):
    pads = {"GND": [(6, 6), (14, 6)], "+3V3": [(10, 3)], "N1": [(9.0, 6)]}
    pads.update(extra or {})
    return make_board(tmp_path, pads)


def test_every_plane_pad_gets_exactly_one_via_within_1mm(tmp_path):
    b = _stub(tmp_path)
    placed, failed = route.fan_out_planes(b)
    assert failed == []
    for fp, p in _pads(b, PLANES):
        near = [x for x in _vias(b, p.GetNetname())
                if _pad_dist(p, x.GetPosition()) <= 1.0]
        assert len(near) == 1, (fp.GetReference(), len(near))
    assert len(_vias(b, "GND")) == 2 and len(_vias(b, "+3V3")) == 1
    assert not _vias(b, "N1")


def test_vias_are_locked_tented_standard_size_and_necked_to_the_pad(tmp_path):
    b = _stub(tmp_path)
    route.fan_out_planes(b)
    for x in _vias(b, "GND") + _vias(b, "+3V3"):
        assert x.IsLocked()
        assert x.GetWidth(F) == pcbnew.FromMM(0.6) and x.GetDrill() == pcbnew.FromMM(0.3)  # P21
        assert x.GetFrontTentingMode() == pcbnew.TENTING_MODE_TENTED
        assert x.GetBackTentingMode() == pcbnew.TENTING_MODE_TENTED
    necks = [t for t in b.GetTracks() if t.GetClass() != "PCB_VIA"]
    assert len(necks) == 3 and all(t.IsLocked() and t.GetLayer() == F for t in necks)
    # each neck joins its pad to its via: the pieces of GND are two (pad + neck + via each)
    assert len(route._components(b, b.FindNet("GND").GetNetCode())) == 2


def test_vias_keep_clear_of_other_nets_pads(tmp_path):
    b = _stub(tmp_path)
    route.fan_out_planes(b)
    for _, p in _pads(b, ("N1",)):
        for x in _vias(b, "GND") + _vias(b, "+3V3"):
            assert _pad_dist(p, x.GetPosition()) - 0.3 >= 0.15


def test_a_boxed_in_pad_takes_the_one_free_side(tmp_path):
    # GND at (6, 6) walled by N1 pads above, below and left, 1.2 mm away: only the right is free
    b = _stub(tmp_path, {"N1": [(9.0, 6), (6, 4.6), (6, 7.4), (4.6, 6)]})
    route.fan_out_planes(b)
    p = next(p for _, p in _pads(b, ("GND",)) if abs(pcbnew.ToMM(p.GetX()) - 6) < 0.1)
    near = [x for x in _vias(b, "GND") if _pad_dist(p, x.GetPosition()) <= 1.0]
    assert len(near) == 1 and near[0].GetX() > p.GetX()


def test_a_pad_with_no_room_is_reported_not_forced(tmp_path):
    walls = [(6 + dx, 6 + dy) for dx in (-1.1, 0, 1.1) for dy in (-1.1, 0, 1.1)
             if (dx, dy) != (0, 0)]
    b = make_board(tmp_path, {"GND": [(6, 6)], "N1": walls, "+3V3": [(20, 3)]})
    placed, failed = route.fan_out_planes(b)
    assert len(failed) == 1 and "GND" in failed[0]
    assert len(_vias(b, "+3V3")) == 1 and not _vias(b, "GND")


def test_a_decoupling_cap_pad_prefers_the_ic_side(tmp_path):
    """Both sides are free; the cap's via goes to the side of the IC pin (shortest loop)."""
    b = make_board(tmp_path, {"+3V3": [(10, 6), (7, 6)], "GND": [(20, 3)]})
    fps = sorted(b.GetFootprints(), key=lambda f: f.GetReference())
    fps[0].SetReference("C1")            # the pad at (10, 6) is the cap, (7, 6) the IC "U1"
    fps[1].SetReference("U1")
    cap, ic = fps[0].Pads()[0], fps[1].Pads()[0]
    assert cap.GetX() > ic.GetX()
    route.fan_out_planes(b)
    mine = [x for x in _vias(b, "+3V3") if _pad_dist(cap, x.GetPosition()) <= 1.0]
    assert any(x.GetX() < cap.GetX() for x in mine), "the cap's via is not on the IC side"


def test_plane_nets_are_the_nets_of_inner_zones(tmp_path):
    b = _stub(tmp_path)
    assert route.plane_nets(b) == set()
    z = pcbnew.ZONE(b)
    z.SetLayer(pcbnew.In1_Cu)
    z.SetNet(b.FindNet("GND"))
    b.Add(z)
    assert route.plane_nets(b) == {"GND"}


def test_finish_leaves_plane_nets_to_their_plane(tmp_path):
    b = _stub(tmp_path)
    z = pcbnew.ZONE(b)
    z.SetLayer(pcbnew.In1_Cu)
    z.SetNet(b.FindNet("GND"))
    b.Add(z)
    assert route.finish_unrouted(b) == []
    assert not [t for t in b.GetTracks() if t.GetNetname() == "GND"]


def test_unlocked_plane_copper_from_the_router_is_dropped(tmp_path):
    b = _stub(tmp_path)
    route.fan_out_planes(b)
    n = len(list(b.GetTracks()))
    t = pcbnew.PCB_TRACK(b)
    t.SetStart(v(6, 6))
    t.SetEnd(v(14, 6))
    t.SetLayer(F)
    t.SetWidth(pcbnew.FromMM(0.15))
    t.SetNet(b.FindNet("GND"))
    b.Add(t)
    assert route.drop_plane_copper(b, {"GND", "+3V3"}) == 1
    assert len(list(b.GetTracks())) == n


def test_the_real_board_fans_out_with_none_in_the_tab(tmp_path):
    b = pcbnew.LoadBoard(str(ROOT / "hw/carrier/carrier.kicad_pcb"))
    route.strip_routing(b)
    placed, failed = route.fan_out_planes(b)
    assert failed == []
    assert check_edge.problems_inner(b) == []        # no via in the tab, none near an edge
    smd = [(fp, p) for fp, p in _pads(b, PLANES) if p.GetAttribute() == pcbnew.PAD_ATTRIB_SMD]
    assert len(placed) == 2 * len(smd)               # a neck and a via for each pad
    assert len(_vias(b, "GND")) + len(_vias(b, "+3V3")) == len(smd)
    for fp, p in smd:
        lay = F if p.IsOnLayer(F) else pcbnew.B_Cu
        near = [x for x in _vias(b, p.GetNetname())
                if _pad_dist(p, x.GetPosition(), lay) <= 1.0]
        assert len(near) >= 1, (fp.GetReference(), p.GetNumber())


def _plane_board(tmp_path):
    b = _stub(tmp_path, {"X": [(3, 9), (21, 9)]})
    for layer, net in ((pcbnew.In1_Cu, "GND"), (pcbnew.In2_Cu, "+3V3")):
        z = pcbnew.ZONE(b)
        z.SetLayer(layer)
        z.SetNet(b.FindNet(net))
        z.SetIsRuleArea(False)
        z.AppendCorner(v(1, 1), -1)
        for x, y in ((23, 1), (23, 11), (1, 11)):
            z.AppendCorner(v(x, y), -1)
        b.Add(z)
    path = tmp_path / "planes.kicad_pcb"
    pcbnew.SaveBoard(str(path), b)
    return path


def test_route_fans_out_first_and_drops_what_the_router_lays_for_a_plane(tmp_path):
    from hw.tests.test_route import _ses
    path = _plane_board(tmp_path)
    seen = {}

    def stub(dsn, ses, passes, timeout_s, log, extra=()):
        text = Path(dsn).read_text()
        seen["fixed"] = text.count("(type fix)")
        # the router lays X, and (wrongly) a GND track as well
        Path(ses).write_text(_ses("X", -9000).replace(
            "(net X", "(net GND (wire (path F.Cu 150  6000 -6000  14000 -6000)))\n      (net X"))
        return 0

    route.route(path, router=stub)
    b = pcbnew.LoadBoard(str(path))
    assert len(_vias(b, "GND")) == 2 and len(_vias(b, "+3V3")) == 1
    planes = [t for t in b.GetTracks() if t.GetNetname() in PLANES]
    assert len(planes) == 6 and all(t.IsLocked() for t in planes)   # three necks, three vias
    assert seen["fixed"] >= 3                                # the router was handed them as fixed
    assert [t for t in b.GetTracks() if t.GetNetname() == "X"]
