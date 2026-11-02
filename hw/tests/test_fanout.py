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


def test_a_pad_that_has_its_via_is_not_fanned_out_again(tmp_path):
    """--keep-locked / --keep-all routes run the fanout over a board that has it already."""
    b = _stub(tmp_path)
    route.fan_out_planes(b)
    n = len(list(b.GetTracks()))
    placed, failed = route.fan_out_planes(b)
    assert placed == [] and failed == [] and len(list(b.GetTracks())) == n


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


def _ic(tmp_path, extra=None):
    """U1 with a GND pad at (10, 6) and a +3V3 pad at (10, 9): its middle is y = 7.5, so
    the GND pad's outer side (where its signals escape) is up (smaller y)."""
    pads = {"GND": [(10, 6)], "+3V3": [(10, 9)]}
    pads.update(extra or {})
    b = make_board(tmp_path, pads)
    fps = {p.GetNetname(): fp for fp in b.GetFootprints() for p in fp.Pads()}
    u, other = fps["GND"], fps["+3V3"]
    u.SetReference("U1")
    pad = other.Pads()[0]
    other.Remove(pad)
    u.Add(pad)
    return b, u


def test_an_ic_pad_puts_its_via_on_the_inner_side_not_where_signals_escape(tmp_path):
    b, u = _ic(tmp_path)
    route.fan_out_planes(b)
    gnd_pad = next(p for p in u.Pads() if p.GetNetname() == "GND")
    (via,) = _vias(b, "GND")
    assert via.GetY() > gnd_pad.GetY()                # toward the middle of the part


def test_an_ic_pad_uses_the_outer_side_when_the_inner_has_no_room(tmp_path):
    b, u = _ic(tmp_path, {"N1": [(9.2, 7.2), (10.8, 7.2), (10, 7.4)]})
    route.fan_out_planes(b)
    gnd_pad = next(p for p in u.Pads() if p.GetNetname() == "GND")
    (via,) = _vias(b, "GND")
    assert via.GetY() < gnd_pad.GetY()


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
    shares: list = []
    placed, failed = route.fan_out_planes(b, shares=shares)
    assert failed == []
    assert check_edge.problems_inner(b) == []        # no via in the tab, none near an edge
    assert _overlaps(b) == []                        # P28: no via on any pad
    smd = [(fp, p) for fp, p in _pads(b, PLANES) if p.GetAttribute() == pcbnew.PAD_ATTRIB_SMD]
    # a neck and a via for each pad, but a share (one track) for a pad with no room (P28)
    assert len(placed) == 2 * (len(smd) - len(shares)) + len(shares)
    assert len(_vias(b, "GND")) + len(_vias(b, "+3V3")) == len(smd) - len(shares)
    shared = {s.split(" -> ")[0] for s in shares}
    for fp, p in smd:
        lay = F if p.IsOnLayer(F) else pcbnew.B_Cu
        near = [x for x in _vias(b, p.GetNetname())
                if _pad_dist(p, x.GetPosition(), lay) <= 1.0]
        assert near or f"{fp.GetReference()}.{p.GetNumber()}" in shared, \
            (fp.GetReference(), p.GetNumber())


def test_route_py_runs_as_a_script_and_finds_the_hw_package():
    """`python3 hw/tools/route.py` (the justfile's way) must import hw.tools.check_edge."""
    import subprocess
    import sys
    r = subprocess.run([sys.executable, str(ROOT / "hw/tools/route.py"), "--help"],
                       capture_output=True, text=True, cwd="/")
    assert r.returncode == 0, r.stderr
    src = (ROOT / "hw/tools/route.py").read_text()
    assert "sys.path.insert" in src


def test_the_first_pass_lays_reset_and_iosel_short_and_locked(tmp_path):
    """P23: the two J1 nets nearest the 15 pF limit are in the default first pass."""
    b = make_board(tmp_path, {"SLOT_RESET_N": [(3, 3), (20, 3)], "SLOT_IOSEL_N": [(3, 6), (20, 6)],
                              "SLOT_D0": [(3, 9), (20, 9)]})
    names = route.default_first(b)
    assert "SLOT_RESET_N" in names and "SLOT_IOSEL_N" in names and "SLOT_D0" in names
    # and the FPGA-side nets of the band, after them
    assert route.BAND_NETS[0] == "slot_d0" and "slot_a3" in route.BAND_NETS
    assert names.index("SLOT_D0") < names.index("SLOT_IOSEL_N")
    assert route.lay_first(b, names) == []
    assert all(t.IsLocked() for t in b.GetTracks())


def test_a_route_leaves_the_project_file_byte_identical(tmp_path):
    """P24: pcbnew.SaveBoard rewrites a .kicad_pro next to the board; route() must not."""
    from hw.tests.test_route import _ses
    path = _plane_board(tmp_path)
    pro = path.with_suffix(".kicad_pro")
    pro.write_bytes((ROOT / "hw/carrier/carrier.kicad_pro").read_bytes())
    before = pro.read_bytes()

    def stub(dsn, ses, passes, timeout_s, log, extra=()):
        Path(ses).write_text(_ses("X", -9000))
        return 0

    route.route(path, router=stub)
    assert pro.read_bytes() == before


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


# --- P28: no via in a pad ----------------------------------------------------------------------

def _overlaps(b):
    """(via, pad) pairs where a via's copper overlaps an SMD pad's (any net)."""
    out = []
    for x in b.GetTracks():
        if x.GetClass() != "PCB_VIA":
            continue
        for fp, p in _pads(b, {n.GetNetname() for n in b.GetNetsByName().values()}):
            if p.GetAttribute() == pcbnew.PAD_ATTRIB_SMD and \
                    _pad_dist(p, x.GetPosition(), F if p.IsOnLayer(F) else pcbnew.B_Cu) < 0.3:
                out.append((x.GetNetname(), fp.GetReference()))
            if p.GetAttribute() == pcbnew.PAD_ATTRIB_SMD and p.HitTest(x.GetPosition()):
                out.append((x.GetNetname(), fp.GetReference(), "inside"))
    return out


def _big(b, x, y, size=1.2):
    """Grow the pad at (x, y) to size x size mm: its centre is then over 0.4 mm from its edge."""
    for fp in b.GetFootprints():
        for p in fp.Pads():
            if abs(pcbnew.ToMM(p.GetX()) - x) < 1e-6 and abs(pcbnew.ToMM(p.GetY()) - y) < 1e-6:
                p.SetSize(v(size, size))
                return fp, p
    raise AssertionError("no pad there")


def _walls(x, y, d=1.3, skip=()):
    return [(x + dx, y + dy) for dx in (-d, 0, d) for dy in (-d, 0, d)
            if (dx, dy) != (0, 0) and (dx, dy) not in skip]


def test_a_via_never_lands_inside_a_pad(tmp_path):
    """P28: a 1.2 mm GND pad boxed in has no room for its via; its own middle (0.6 mm from its
    edge) is not room: the pad is reported, and no via overlaps a pad."""
    b = make_board(tmp_path, {"GND": [(6, 6)], "N1": _walls(6, 6), "+3V3": [(20, 3)]})
    _big(b, 6, 6)
    placed, failed = route.fan_out_planes(b)
    assert _overlaps(b) == []
    assert len(failed) == 1 and "GND" in failed[0]


def test_a_pad_with_no_room_shares_a_same_net_neighbours_via(tmp_path):
    """P28: a boxed-in GND pad 0.5 mm from another GND pad that has room is joined to it by a
    short locked track and shares its via; nothing is reported, no via sits in a pad."""
    b = make_board(tmp_path, {"GND": [(6, 6), (7.5, 6)], "N1": _walls(6, 6, skip={(1.3, 0)}),
                              "+3V3": [(20, 3)]})
    fp, boxed = _big(b, 6, 6)
    placed, failed = route.fan_out_planes(b)
    assert failed == []
    assert _overlaps(b) == []
    assert len(_vias(b, "GND")) == 1
    gnd = b.FindNet("GND").GetNetCode()
    assert len(route._components(b, gnd)) == 1             # boxed pad, track, neighbour, via
    joins = [t for t in b.GetTracks() if t.GetClass() != "PCB_VIA" and t.GetNetname() == "GND"
             and boxed.HitTest(t.GetStart()) or (t.GetClass() != "PCB_VIA" and boxed.HitTest(t.GetEnd()))]
    assert joins and all(t.IsLocked() for t in joins)
    assert all(pcbnew.ToMM(t.GetLength()) <= 1.5 for t in joins)


def test_a_pad_with_no_room_and_no_neighbour_is_still_reported(tmp_path):
    b = make_board(tmp_path, {"GND": [(6, 6)], "N1": _walls(6, 6), "+3V3": [(20, 3)]})
    placed, failed = route.fan_out_planes(b)
    assert len(failed) == 1 and "GND" in failed[0]


def test_the_finisher_grid_puts_no_via_on_its_own_nets_pads(tmp_path):
    """P28 for the A* (first pass, finisher): a via may not be laid over a pad of the net it
    joins, even though its tracks may run onto that pad."""
    b = make_board(tmp_path, {"GND": [(6, 6)], "N1": [(12, 6), (18, 6)]})
    pad = next(p for fp in b.GetFootprints() for p in fp.Pads() if p.GetNetname() == "N1")
    g = route._Grid(b, pad.GetNetCode(), 0.15)
    i, j = g.px(pad.GetX(), pad.GetY())
    assert g.via[int(j), int(i)]                   # no via on the pad
    assert not g.track[F][int(j), int(i)]          # but a track may reach it
