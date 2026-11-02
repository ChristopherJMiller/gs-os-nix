# SPDX-License-Identifier: GPL-3.0-or-later
"""bus_route (Task 5g) on small hand-made boards: a river is laid planar, DRC-clean, locked and
the same on every run; what it cannot pass is reported, never forced; a P19 plane via in its way
moves; the Clock class keeps its clearance."""
import itertools
import random
from pathlib import Path

import pcbnew
import pytest

from hw.tools import bus_route as br
from hw.tools import check_drc
from hw.tools import route

F, B = pcbnew.F_Cu, pcbnew.B_Cu
N = 8


def v(x, y):
    return pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y))


def make(w=30.0, h=30.0, clocks=()):
    """An empty w x h mm board with the carrier's classes: Default 0.15/0.15, Clock 0.2/0.3."""
    b = pcbnew.BOARD()
    for a, c in [((0, 0), (w, 0)), ((w, 0), (w, h)), ((w, h), (0, h)), ((0, h), (0, 0))]:
        s = pcbnew.PCB_SHAPE(b)
        s.SetShape(pcbnew.SHAPE_T_SEGMENT)
        s.SetLayer(pcbnew.Edge_Cuts)
        s.SetWidth(pcbnew.FromMM(0.1))
        s.SetStart(v(*a))
        s.SetEnd(v(*c))
        b.Add(s)
    ns = b.GetDesignSettings().m_NetSettings
    default = ns.GetDefaultNetclass()
    default.SetTrackWidth(pcbnew.FromMM(0.15))
    default.SetClearance(pcbnew.FromMM(0.15))
    clock = pcbnew.NETCLASS("Clock")
    clock.SetTrackWidth(pcbnew.FromMM(0.2))
    clock.SetClearance(pcbnew.FromMM(0.3))
    ns.SetNetclass("Clock", clock)
    for n in clocks:
        ns.SetNetclassPatternAssignment(n, "Clock")
    ds = b.GetDesignSettings()
    ds.m_MinClearance = pcbnew.FromMM(0.15)
    ds.m_TrackMinWidth = pcbnew.FromMM(0.15)
    ds.m_CopperEdgeClearance = pcbnew.FromMM(0.5)
    part(b, "G1", [(1, "GND", w - 3, h - 3, 0.8, 0.8)])   # route.finish_unrouted joins GND
    return b


def net(b, name):
    if b.FindNet(name) is None:
        b.Add(pcbnew.NETINFO_ITEM(b, name))
    return b.FindNet(name)


def part(b, ref, pads):
    """A footprint `ref` of SMD pads on F.Cu: [(number, net, x, y, w, h)]."""
    fp = pcbnew.FOOTPRINT(b)
    fp.SetReference(ref)
    fp.SetPosition(v(pads[0][2], pads[0][3]))
    for num, n, x, y, pw, ph in pads:
        pad = pcbnew.PAD(fp)
        pad.SetNumber(str(num))
        pad.SetShape(pcbnew.PAD_SHAPE_RECT)
        pad.SetAttribute(pcbnew.PAD_ATTRIB_SMD)
        pad.SetLayerSet(pad.SMDMask())
        pad.SetSize(v(pw, ph))
        pad.SetPosition(v(x, y))
        if n:
            pad.SetNet(net(b, n))
        fp.Add(pad)
    b.Add(fp)
    b.GetDesignSettings().m_NetSettings.RecomputeEffectiveNetclasses()
    return fp


def river_board(n=N, obstacle=True, plane=False, clock_track=False):
    """n nets leave a row of 0.3 mm pads (U1, 0.5 mm pitch, y 22) for a row of 0.35 x 2 mm pads
    (J2, 0.6 mm pitch, y 8) 2 mm to the right. Optional: a pad of another net just above U1
    between pins 3 and 4 (where the lanes must part), a +3V3 pad with its P19 via in the way,
    a Clock-class track the river must keep 0.3 mm from."""
    b = make(clocks=("CLK",))
    for k in range(n):
        net(b, f"N{k}")
    part(b, "U1", [(k + 1, f"N{k}", 8 + 0.5 * k, 22, 0.3, 1.48) for k in range(n)])
    part(b, "J2", [(k + 1, f"N{k}", 10 + 0.6 * k, 8, 0.35, 2.0) for k in range(n)])
    if obstacle:
        part(b, "C1", [(1, "OB", 9.75, 19.8, 0.6, 0.6)])
    if plane:                     # a +3V3 pad over pins 6 and 7, its P19 via on their exits
        part(b, "C2", [(1, "+3V3", 11.25, 19.4, 0.9, 0.6)])
        z = pcbnew.ZONE(b)                         # the +3V3 plane, so the via has a plane net
        z.SetLayer(pcbnew.In2_Cu)
        z.SetNet(net(b, "+3V3"))
        o = z.Outline()
        o.NewOutline()
        for x, y in ((1, 1), (29, 1), (29, 29), (1, 29)):
            o.Append(pcbnew.FromMM(x), pcbnew.FromMM(y))
        b.SetCopperLayerCount(4)
        b.Add(z)
        for item in (br.Seg(F, (11.25, 19.5), (11.25, 20.45), 0.3), br.Via((11.25, 20.45))):
            br.lay_item(b, "+3V3", item)           # as fan_out_planes lays them: locked
    if clock_track:               # a Clock-class piece 0.245 mm from N7's straight run
        t = pcbnew.PCB_TRACK(b)
        t.SetStart(v(11.92, 17.3))
        t.SetEnd(v(11.92, 20.7))
        t.SetLayer(F)
        t.SetWidth(pcbnew.FromMM(0.2))
        t.SetNet(net(b, "CLK"))
        t.SetLocked(True)
        b.Add(t)
    b.GetDesignSettings().m_NetSettings.RecomputeEffectiveNetclasses()
    return b


def lay_river(b, y0=17.0):
    r = br.Router(b)
    u1, j2 = b.FindFootprintByReference("U1"), b.FindFootprintByReference("J2")
    members = [(p.GetNetname(), p, next(q for q in j2.Pads() if q.GetNetname() == p.GetNetname()))
               for p in u1.Pads()]
    br.vriver(r, "test", members, y0=y0)
    r.relocate()
    r.lay()
    return r.report


def drc(b, tmp_path, name="b"):
    path = tmp_path / f"{name}.kicad_pcb"
    pcbnew.SaveBoard(str(path), b)
    return check_drc.run_drc(path)


def river_nets(b):
    return sorted(str(n) for n in b.GetNetsByName().keys() if str(n).startswith("N"))


def _crossings(b) -> int:
    """Pairs of segments of different nets on one layer that cross or touch."""
    segs = [t for t in b.GetTracks() if t.GetClass() != "PCB_VIA"]
    n = 0
    for s, t in itertools.combinations(segs, 2):
        if s.GetNetCode() != t.GetNetCode() and s.GetLayer() == t.GetLayer():
            d = br.seg_seg(br._pt(s.GetStart()), br._pt(s.GetEnd()),
                           br._pt(t.GetStart()), br._pt(t.GetEnd()))
            n += d < pcbnew.ToMM(s.GetWidth() + t.GetWidth()) / 2
    return n


# --- the lanes and the rows -----------------------------------------------------------------

def _brute_lanes(xs, intervals, gap, reach, grid):
    best = None
    for combo in itertools.product(grid, repeat=len(xs)):
        if any(b - a < gap - 1e-9 for a, b in zip(combo, combo[1:])):
            continue
        if any(lo < p < hi for p in combo for lo, hi, _ in intervals):
            continue
        if any(abs(p - x) > reach + 1e-9 for p, x in zip(combo, xs)):
            continue
        cost = sum(abs(p - x) for p, x in zip(combo, xs))
        if best is None or cost < best - 1e-9:
            best = cost
    return best


def test_lanes_are_ordered_spaced_clear_and_least_displaced():
    rng = random.Random(3)
    for _ in range(40):
        n = rng.randint(1, 3)
        xs = sorted(round(rng.uniform(1, 3), 1) for _ in range(n))
        xs = [x + 0.5 * k for k, x in enumerate(xs)]
        a = round(rng.uniform(1, 4), 1)
        intervals = [(a, a + round(rng.uniform(0.2, 0.8), 1), a + 0.3)]
        px = br.lanes(xs, intervals, 0.5, reach=2.0)
        grid = [round(0.1 * k, 2) for k in range(-10, 81)]
        best = _brute_lanes(xs, intervals, 0.5, 2.0, grid)
        if best is None:
            assert None in px
            continue
        assert None not in px
        assert all(b - a_ >= 0.5 - 1e-9 for a_, b in zip(px, px[1:]))
        assert not any(lo < p < hi for p in px for lo, hi, _ in intervals)
        assert sum(abs(p - x) for p, x in zip(px, xs)) == pytest.approx(best, abs=0.011 * n)


def test_lanes_take_either_side_of_an_obstacle():
    """Which side of an obstacle a lane passes is the programme's choice, not the obstacle's
    centre: two nets left of an obstacle's centre both pass right of it when a wall closes its
    left side (a rule by centres would push them into the wall)."""
    px = br.lanes([5.0, 5.2], [(0.0, 4.95, 2.0), (4.8, 5.6, 5.4)], 0.46, reach=3.0)
    assert px[0] is not None and px[1] is not None
    assert px[0] >= 5.6 and px[1] >= px[0] + 0.46 - 1e-9


def test_lanes_leave_out_a_net_that_cannot_fit():
    px = br.lanes([5.0, 5.1], [(4.0, 6.0, 5.0)], 0.46, reach=0.5)
    assert px == [None, None]


def test_rows_nest_the_outer_net_nearest_the_end():
    """Right-movers: the leftmost start turns nearest the end (row 0) and wraps round the
    others; spans that do not overlap share a row; a blocked row is skipped."""
    spans = [("A", 0.0, 10.0, 0.0), ("B", 2.0, 10.5, 2.0), ("C", 20.0, 22.0, 20.0)]
    rows = br.rows(spans, lambda k, a, b: False, 8)
    assert rows == {"A": 0, "B": 1, "C": 0}
    rows = br.rows(spans, lambda k, a, b: k == 0 and a < 15, 8)
    assert rows["A"] == 1 and rows["B"] == 2 and rows["C"] == 0


# --- a river on a stub board ----------------------------------------------------------------

def test_a_river_is_whole_planar_locked_and_drc_clean(tmp_path):
    b = river_board()
    report = lay_river(b)
    assert report.left == {}
    assert sorted(report.laid["test"]) == sorted(river_nets(b))
    for n in river_nets(b):
        assert len(route._components(b, b.FindNet(n).GetNetCode())) == 1, n
    tracks = list(b.GetTracks())
    assert tracks and all(t.IsLocked() for t in tracks)
    assert _crossings(b) == 0
    rep = drc(b, tmp_path)
    assert rep["violations"] == [], rep["violations"][:3]
    assert not [u for u in rep["unconnected_items"]
                if any("[N" in i.get("description", "") for i in u.get("items", []))]


def test_the_lanes_part_round_the_obstacle(tmp_path):
    """The pad between pins 3 and 4 sends 0-3 left and 4-7 right; nothing runs over it."""
    b = river_board()
    lay_river(b)
    ob = next(p for fp in b.GetFootprints() for p in fp.Pads() if p.GetNetname() == "OB")
    ox = pcbnew.ToMM(ob.GetX())
    oy = pcbnew.ToMM(ob.GetY())
    for k in range(N):               # where each net crosses the obstacle's height
        xs = []
        for t in b.GetTracks():
            if t.GetNetname() == f"N{k}" and t.GetClass() != "PCB_VIA":
                (x0, y0), (x1, y1) = br._pt(t.GetStart()), br._pt(t.GetEnd())
                if min(y0, y1) <= oy <= max(y0, y1) and y0 != y1:
                    xs.append(x0 + (x1 - x0) * (oy - y0) / (y1 - y0))
        assert len(xs) == 1 and (xs[0] < ox) == (k < 4), (k, xs)


def test_two_runs_lay_byte_identical_copper():
    a, b = river_board(), river_board()
    lay_river(a)
    lay_river(b)
    assert br.signature(a) == br.signature(b)
    assert "track N0" in br.signature(a)


def test_an_obstacle_it_cannot_pass_is_reported_not_forced(tmp_path):
    """A pad of another net across the whole river on its way up to J2 (beyond the lanes,
    where only the clearance check sees it): every net is reported with it, none is laid."""
    b = river_board(obstacle=False)
    part(b, "W1", [(1, "WALL", 13.0, 12.0, 16.0, 0.6)])
    report = lay_river(b)
    assert sorted(report.left) == river_nets(b)
    assert all("W1.1" in why for why in report.left.values()), report.left
    assert not list(b.GetTracks())
    assert drc(b, tmp_path)["violations"] == []


def test_a_wall_across_the_lanes_is_reported_too():
    b = river_board(obstacle=False)
    part(b, "W1", [(1, "WALL", 12.0, 19.5, 16.0, 0.6)])       # across the lanes' band
    report = lay_river(b)
    assert sorted(report.left) == river_nets(b)
    assert all("lane" in why for why in report.left.values()), report.left
    assert not list(b.GetTracks())


def test_a_net_it_cannot_lay_whole_leaves_nothing_behind():
    """A net is kept whole or not at all: a pad dropped on one net's lane after the lanes are
    chosen (the check is what refuses it) leaves no stub of that net on the board."""
    b = river_board(obstacle=False)
    r = br.Router(b)
    u1, j2 = b.FindFootprintByReference("U1"), b.FindFootprintByReference("J2")
    part(b, "W2", [(1, "WALL", 10 + 0.6 * 5, 12.0, 0.3, 0.3)])   # on N5's last straight
    r.space = br.Space(b)
    members = [(p.GetNetname(), p, next(q for q in j2.Pads() if q.GetNetname() == p.GetNetname()))
               for p in u1.Pads()]
    br.vriver(r, "test", members, y0=17.0)
    r.lay()
    assert "N5" in r.report.left and "W2.1" in r.report.left["N5"]
    assert not [t for t in b.GetTracks() if t.GetNetname() == "N5"]
    assert len([n for n in river_nets(b) if n != "N5"]) == len(r.report.laid["test"])


def test_a_plane_via_in_a_lane_moves_and_its_pad_keeps_a_via_within_1mm(tmp_path):
    b = river_board(plane=True)
    before = [br._pt(t.GetPosition()) for t in b.GetTracks() if t.GetClass() == "PCB_VIA"]
    report = lay_river(b)
    assert report.left == {}
    assert report.moved and report.moved[0].startswith("C2.1")
    vias = [t for t in b.GetTracks() if t.GetClass() == "PCB_VIA" and t.GetNetname() == "+3V3"]
    assert len(vias) == 1 and br._pt(vias[0].GetPosition()) != before[0]
    pad = b.FindFootprintByReference("C2").Pads()[0]
    d = min(br.seg_poly(br._pt(vias[0].GetPosition()), br._pt(vias[0].GetPosition()), poly)
            for poly in br.pad_poly(pad, F))
    assert route.PLANE_VIA_MIN_MM - 1e-6 <= d <= route.PLANE_VIA_REACH_MM + 1e-6
    assert vias[0].IsLocked()
    assert len(route._components(b, b.FindNet("+3V3").GetNetCode())) == 1   # pad, neck, via
    assert drc(b, tmp_path)["violations"] == []


def test_the_river_keeps_the_clock_clearance(tmp_path):
    """A Clock-class track stands 0.245 mm (edge to edge) right of N7's straight run up
    (enough for the 0.15 mm rule): N7 moves over to keep the Clock class's 0.3 mm, and KiCad's DRC
    agrees."""
    b = river_board(obstacle=False, clock_track=True)
    report = lay_river(b)
    assert report.left == {}
    clk = next(t for t in b.GetTracks() if t.GetNetname() == "CLK")
    for t in b.GetTracks():
        if t.GetNetname().startswith("N") and t.GetClass() != "PCB_VIA" and t.GetLayer() == F:
            d = br.seg_seg(br._pt(t.GetStart()), br._pt(t.GetEnd()),
                           br._pt(clk.GetStart()), br._pt(clk.GetEnd()))
            assert d - 0.075 - 0.1 >= 0.3 - 1e-6, (t.GetNetname(), d)
    assert drc(b, tmp_path)["violations"] == []


def test_a_river_with_rows_nests_and_is_drc_clean(tmp_path):
    """lriver: starts 2.54 mm apart below (fingers), ends 0.5 mm apart above, compressing on
    both sides; nested rows, no crossing, DRC-clean."""
    b = make(w=40, h=30)
    n = 8
    part(b, "J1", [(k + 1, f"N{k}", 4 + 2.54 * k, 26, 1.0, 3.0) for k in range(n)])
    part(b, "U1", [(k + 1, f"N{k}", 12.5 + 0.5 * k, 10, 0.3, 1.48) for k in range(n)])
    b.GetDesignSettings().m_NetSettings.RecomputeEffectiveNetclasses()
    r = br.Router(b)
    u1, j1 = b.FindFootprintByReference("U1"), b.FindFootprintByReference("J1")
    members = [(p.GetNetname(), (br._mm(p.GetX()), br._mm(p.GetY()) - 1.0),
                br._pt(next(q for q in u1.Pads() if q.GetNetname() == p.GetNetname()).GetPosition()))
               for p in j1.Pads()]
    pens = br.lriver(r, "rows", members, layer=F, row_top=14.0)
    for net_, pen in sorted(pens.items()):
        r.offer(net_, "rows", pen.items)
    r.lay()
    assert r.report.left == {} and len(r.report.laid["rows"]) == n
    assert _crossings(b) == 0
    assert drc(b, tmp_path)["violations"] == []


# --- route.py runs it -----------------------------------------------------------------------

EMPTY_SES = """(session x.ses
  (base_design x.dsn)
  (placement (resolution um 1))
  (was_is)
  (routes
    (resolution um 1)
    (parser (host_cad "KiCad's Pcbnew") (host_version "x"))
    (library_out)
    (network_out)
  )
)
"""


def _test_river(r):
    u1, j2 = r.board.FindFootprintByReference("U1"), r.board.FindFootprintByReference("J2")
    members = [(p.GetNetname(), p, next(q for q in j2.Pads() if q.GetNetname() == p.GetNetname()))
               for p in u1.Pads()]
    br.vriver(r, "test", members, y0=17.0)


def test_route_lays_the_buses_before_the_router_and_hands_them_over_fixed(tmp_path):
    import re
    path = tmp_path / "r.kicad_pcb"
    pcbnew.SaveBoard(str(path), river_board(obstacle=False))
    seen = {}

    def stub(dsn, ses, passes, timeout_s, log, extra=()):
        text = Path(dsn).read_text()
        seen["fixed"] = sorted(set(re.findall(r"\(net (N\d)\)\(type fix\)", text)))
        Path(ses).write_text(EMPTY_SES)
        return 0

    route.route(path, router=stub, bus=[("test", _test_river)])
    assert seen["fixed"] == [f"N{k}" for k in range(N)]
    b = pcbnew.LoadBoard(str(path))
    mine = [t for t in b.GetTracks() if t.GetNetname().startswith("N")]
    assert mine and all(t.IsLocked() for t in mine)


def test_a_continued_route_does_not_lay_a_river_twice(tmp_path):
    """route --keep-locked keeps a river laid before; bus_route leaves a net that has copper."""
    b = river_board(obstacle=False)
    lay_river(b)
    n = len(list(b.GetTracks()))
    report = lay_river(b)
    assert len(list(b.GetTracks())) == n
    assert sorted(report.kept) == river_nets(b)


# --- the carrier ---------------------------------------------------------------------------

ROOT = Path(__file__).resolve().parents[2]
CARRIER = ROOT / "hw" / "carrier" / "carrier.kicad_pcb"
RIVER_SIZES = {"data": 9, "listen": 8, "address": 17, "control": 7, "slot_data": 8,
               "slot_address": 16}


@pytest.fixture(scope="module")
def carrier(tmp_path_factory):
    """bus_route alone (the rivers, not the clocks) on a copy of the committed carrier, after
    the P19 fanout, as route.py runs it."""
    d = tmp_path_factory.mktemp("carrier")
    for suffix in (".kicad_pcb", ".kicad_pro", ".kicad_dru"):
        (d / f"carrier{suffix}").write_bytes(CARRIER.with_suffix(suffix).read_bytes())
    path = d / "carrier.kicad_pcb"
    b = pcbnew.LoadBoard(str(path))
    br.prepare(b)
    report = br.run(b, clocks=False)
    route.save_board(b, path)
    return path, report


def test_bus_route_lays_every_planned_net_of_the_carrier(carrier):
    _, report = carrier
    assert report.left == {}
    assert {k: len(v) for k, v in report.laid.items()} == RIVER_SIZES


def test_the_carrier_copy_is_drc_clean_and_its_planes_whole(carrier):
    path, report = carrier
    rep = check_drc.run_drc(path)
    assert rep["violations"] == [], rep["violations"][:3]
    laid = {n for nets in report.laid.values() for n in nets}
    loose = {i.get("description", "") for u in rep["unconnected_items"] for i in u.get("items", [])}
    for n in ("GND", "+3V3"):
        assert not [d for d in loose if f"[{n}]" in d], n
    # the river nets with two pads are whole; slot_a_oe_n and slot_d_dir keep their pull-up /
    # pull-down and a second pin for the router
    whole = laid - {"slot_a_oe_n", "slot_d_dir"}
    assert not [d for d in loose if any(f"[{n}]" in d for n in whole)]


def test_every_plane_pad_keeps_a_via_within_1mm_after_the_moves(carrier):
    path, report = carrier
    assert report.moved                       # C8 and C9 sit in the address river's way
    b = pcbnew.LoadBoard(str(path))
    vias = {n: [br._pt(t.GetPosition()) for t in b.GetTracks()
                if t.GetClass() == "PCB_VIA" and t.GetNetname() == n] for n in ("GND", "+3V3")}
    for fp in b.GetFootprints():
        for p in fp.Pads():
            if p.GetNetname() in vias and p.GetAttribute() == pcbnew.PAD_ATTRIB_SMD:
                layer = F if p.IsOnLayer(F) else B
                polys = br.pad_poly(p, layer)
                near = [q for q in vias[p.GetNetname()]
                        if min(br.seg_poly(q, q, poly) for poly in polys) <= 1.0 + 1e-6]
                assert near, (fp.GetReference(), p.GetNumber())


def test_route_hands_the_router_the_halo_of_the_clocks_bus_route_laid(tmp_path):
    """bus_route lays the Clock-class nets (by class, after the rivers); route gives the router
    their 0.3 mm as fat fixed wires, with or without a first pass."""
    b = river_board(obstacle=False)
    part(b, "CK", [(1, "CLK", 25, 6, 0.6, 0.6), (2, "CLK", 25, 20, 0.6, 0.6)])
    path = tmp_path / "c.kicad_pcb"
    pcbnew.SaveBoard(str(path), b)
    seen = {}

    def stub(dsn, ses, passes, timeout_s, log, extra=()):
        text = Path(dsn).read_text()
        seen["halo"] = any("(path F.Cu 460 " in line and "(net CLK)" in line
                           for line in text.splitlines())
        Path(ses).write_text(EMPTY_SES)
        return 0

    route.route(path, router=stub, bus=[("test", _test_river)])
    b = pcbnew.LoadBoard(str(path))
    clk = [t for t in b.GetTracks() if t.GetNetname() == "CLK"]
    assert clk and all(t.IsLocked() and t.GetWidth() == pcbnew.FromMM(0.2) for t in clk)
    assert seen["halo"]
