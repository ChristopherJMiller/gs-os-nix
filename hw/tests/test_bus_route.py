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
from hw.tools import plan_buses as pb
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


def _test_layout(r, river):
    j2 = r.board.FindFootprintByReference("J2")
    members = [(n, r.pad(n, river.ref),
                next(q for q in j2.Pads() if q.GetNetname() == n)) for n in river.nets]
    br.vriver(r, river.name, members, y0=17.0)


TEST_RIVER = pb.River("test", "U1", tuple(f"N{k}" for k in range(N)), pb.FRONT, pb.BELOW_F)
TEST_PLAN = [(TEST_RIVER, _test_layout)]


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

    route.route(path, router=stub, bus=TEST_PLAN)
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
NOT_ATTEMPTED = {"slot_d_oe_n", "uart_tx", "uart_rx", "safe_n", "slot_rw", "slot_a_dir",
                 "slot_inh_n_o", "slot_rdy_o", "slot_reset_n_o", "slot_dma_n_o",
                 "slot_irq_n_o", "slot_nmi_n_o"}
CLOCKS = {"slot_ph0", "slot_ph1", "slot_q3", "slot_7m"}


@pytest.fixture(scope="module")
def carrier(tmp_path_factory):
    """bus_route alone (the rivers, not the clocks) on a copy of the committed carrier, after
    the P19 fanout, as route.py runs it."""
    d = tmp_path_factory.mktemp("carrier")
    for suffix in (".kicad_pcb", ".kicad_pro", ".kicad_dru"):
        (d / f"carrier{suffix}").write_bytes(CARRIER.with_suffix(suffix).read_bytes())
    path = d / "carrier.kicad_pcb"
    b = pcbnew.LoadBoard(str(path))
    shares = route.start(b)
    report = br.run(b, clocks=False)
    route.save_board(b, path)
    return path, report, shares


def test_bus_route_lays_every_planned_net_of_the_carrier(carrier):
    _, report, _ = carrier
    assert {k: len(v) for k, v in report.laid.items() if k != "power"} == RIVER_SIZES


def test_every_planned_net_is_laid_or_listed_left(carrier):
    """One source of truth: bus_route walks plan_buses' rivers; a net it does not attempt is
    reported with the reason, never dropped in silence."""
    _, report, _ = carrier
    planned = {n for r in pb.RIVERS + pb.SLOT_RIVERS for n in r.nets}
    laid = {n for k, nets in report.laid.items() if k != "power" for n in nets}
    left = set(report.left) & planned
    assert planned == laid | left
    assert left == NOT_ATTEMPTED | CLOCKS                        # the clocks were off here
    assert all("not attempted" in report.left[n] for n in NOT_ATTEMPTED), report.left
    assert all("clock" in report.left[n] for n in CLOCKS)


def test_the_carrier_copy_is_drc_clean_and_its_planes_whole(carrier):
    path, report, _ = carrier
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
    """P19 and P28: a via within 1 mm of every plane pad, or a share with a same-net pad."""
    path, report, shares = carrier
    assert report.moved                       # C8 and C9 sit in the address river's way
    shared = {s.split(" -> ")[0] for s in shares}
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
                assert near or f"{fp.GetReference()}.{p.GetNumber()}" in shared, \
                    (fp.GetReference(), p.GetNumber())


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

    route.route(path, router=stub, bus=TEST_PLAN)
    b = pcbnew.LoadBoard(str(path))
    clk = [t for t in b.GetTracks() if t.GetNetname() == "CLK"]
    assert clk and all(t.IsLocked() and t.GetWidth() == pcbnew.FromMM(0.2) for t in clk)
    assert seen["halo"]


# --- the plan is plan_buses' (one source of truth) -------------------------------------------

def _pin_board():
    """A TSSOP-like U5 (two rows of 0.4 x 1.48 mm pins at 0.65 mm) with eight signal pins facing
    down; under its pin A3, 1.55 mm below the pin ends, the series resistor R4 of the clock CLK,
    which must reach a pad above the part: the carrier's U5, R3/R4 and slot_q3/slot_7m."""
    b = make(w=30, h=30, clocks=("CLK",))
    xs = [8 + 0.65 * k for k in range(8)]
    part(b, "U5", [(k + 1, f"A{k}", x, 16.0, 0.4, 1.48) for k, x in enumerate(xs)]
         + [(16 - k, f"T{k}", x, 10.28, 0.4, 1.48) for k, x in enumerate(xs)])
    x3 = xs[3] + 0.23                                     # as R4 sits under U5.7, a little aside
    part(b, "R4", [(2, "CLK", x3, 18.76, 0.8, 0.95), (1, "CLKB", x3, 20.41, 0.8, 0.95)])
    part(b, "J2", [(1, "CLK", x3, 4.0, 0.35, 2.0)])
    return b, xs


def test_a_clock_leaves_the_first_millimetre_out_of_every_other_ic_pin_free():
    """The Clock class's 0.3 mm walled a pin in on the carrier: the A* put slot_7m's via 0.6 mm
    below U5.7's end, between its neighbours' tracks, and SLOT_IOSTRB_N could not leave the pin
    within 0.3 mm of it. A clock keeps its clearance from a 0.15 mm track straight out of each
    other signal net's IC pin, ESCAPE_MM long, so every pin can at least leave."""
    b, xs = _pin_board()
    assert route.lay_first(b, ["CLK"]) == []
    clk = [t for t in b.GetTracks() if t.GetNetname() == "CLK"]
    assert any(t.GetClass() == "PCB_VIA" for t in clk)
    end = 16.0 + 0.74
    for k, x in enumerate(xs):
        stub = (v(x, end), v(x, end + route.ESCAPE_MM))
        for t in clk:
            if t.GetClass() != "PCB_VIA" and t.GetLayer() != F:
                continue
            a0, a1, r = route._shape(t)
            gap = route._seg_dist(a0, a1, *stub) - r - route.MIN_TRACK_MM / 2
            assert gap >= 0.3 - 1e-3, (f"A{k}", t.GetClass(), gap)


def test_the_carrier_plan_is_plan_buses_rivers():
    plan = br.carrier_plan()
    assert [river for river, _ in plan] == list(pb.RIVERS + pb.SLOT_RIVERS + br.POWER)
    laid_by = {river.name for river, layout in plan if callable(layout)}
    assert laid_by == set(RIVER_SIZES) | {r.name for r in br.POWER}


def test_a_layout_lays_the_nets_its_river_names():
    """The listen layout given a river of four of U4's eight nets lays those four, no more."""
    b = pcbnew.LoadBoard(str(CARRIER))
    route.start(b)
    four = pb.River("listen", "U4", tuple(f"slot_dl{i}" for i in range(4)), pb.REAR, pb.BELOW_B)
    report = br.run(b, [(four, br.LAYOUTS["listen"])], clocks=False)
    assert report.laid == {"listen": list(four.nets)}
    assert {t.GetNetname() for t in b.GetTracks() if t.GetNetname().startswith("slot_dl")} \
        == set(four.nets)


def test_a_layout_refuses_a_river_reaching_j2_another_way():
    b = river_board(obstacle=False)
    wrong = pb.River("listen", "U1", TEST_RIVER.nets, pb.FRONT, pb.BELOW_F)
    report = br.run(b, [(wrong, br.LAYOUTS["listen"])], clocks=False)
    assert set(report.left) == set(wrong.nets)
    assert all("river skipped" in why and "below-B" in why for why in report.left.values())


def test_a_skipped_river_lists_its_nets():
    b = river_board(obstacle=False)
    missing = pb.River("address", "U9", ("N0", "N1"), pb.FRONT, pb.BELOW_F)    # no U9
    report = br.run(b, [(missing, br.LAYOUTS["address"])], clocks=False)
    assert set(report.left) == {"N0", "N1"}
    assert all("river skipped" in why and "U9" in why for why in report.left.values())


def test_a_layout_that_stops_after_laying_some_nets_lists_only_the_rest():
    """A layout that lays part of its river and then raises CannotLay keeps what it laid, and
    the report lists only the nets it did not lay (5f+5g review, minor 2)."""
    b = river_board(obstacle=False)

    def half(r, river):
        u1, j2 = r.part("U1"), r.part("J2")
        members = sorted((p.GetNetname(), p, next(q for q in j2.Pads()
                                                  if q.GetNetname() == p.GetNetname()))
                         for p in u1.Pads() if p.GetNetname() in river.nets[:4])
        br.vriver(r, river.name, members, y0=17.0)
        raise br.CannotLay("no room for the other four")

    report = br.run(b, [(TEST_RIVER, half)], clocks=False)
    laid = set(report.laid["test"])
    assert laid == {"N0", "N1", "N2", "N3"}
    assert set(report.left) == set(TEST_RIVER.nets) - laid, report.left
    assert {t.GetNetname() for t in b.GetTracks()} == laid


def test_a_net_laid_in_pieces_before_the_layout_stops_is_not_listed_as_left():
    """+5V's groups are joined first, then U2 is looked for: with no U2 on the board the pieces
    stay laid, and the report names what was not laid ("+5V rest"), not the net as unlaid."""
    b = make()
    part(b, "D1", [(1, "+5V", 10, 10, 0.8, 0.8)])
    part(b, "C2", [(1, "+5V", 11.6, 10, 0.8, 0.8)])
    report = br.run(b, [(br.POWER[1], br.POWER_LAYOUTS["+5V"])], clocks=False)
    assert report.laid["power"] == ["+5V C2.1-D1.1"] or report.laid["power"] == ["+5V D1.1-C2.1"]
    assert "+5V" not in report.left, report.left
    assert "U2" in report.left["+5V rest"]


def test_the_not_attempted_reasons_say_who_lays_the_nets_in_whole_sentences():
    """5f+5g review, minor 3: a reason that stopped at "...: the first pass's" read as cut off.
    Each says who lays the river's nets (the first pass or Freerouting) and why, as a sentence."""
    for name, why in br.NOT_ATTEMPTED.items():
        assert why.endswith(".") and not why.rstrip(".").endswith("'s"), (name, why)
        assert "first pass" in why or "Freerouting" in why, (name, why)


def test_the_power_joins_left_to_freerouting_claim_no_order():
    """5f+5g review, minor 1: Freerouting routes every net it is handed together, so the report
    may not say the +5V joins come "after the single nets"."""
    b = make()
    part(b, "U2", [(7, "+5V", 8, 10, 0.3, 1.5), (18, "+5V", 8, 20, 0.3, 1.5)])
    part(b, "C2", [(1, "+5V", 20, 15, 0.8, 0.8)])
    report = br.run(b, [(br.POWER[1], br.POWER_LAYOUTS["+5V"])], clocks=False)
    why = report.left["+5V joins"]
    assert "Freerouting" in why and "after the single" not in why, why


def test_a_coding_bug_in_a_layout_is_not_swallowed():
    def broken(r, river):
        return river.no_such_field
    with pytest.raises(AttributeError):
        br.run(river_board(obstacle=False), [(TEST_RIVER, broken)], clocks=False)


# --- a moved via stays outside its pad; no via in the tab ------------------------------------

def test_a_moved_via_is_offered_no_spot_on_or_by_its_pad():
    """A 1.6 mm pad's middle is 0.8 mm from its edge: a spot there would pass a distance-to-edge
    test. via_spots offers none on the pad and none nearer its outline than P19's 0.4 mm."""
    b = make()
    part(b, "C9", [(1, "+3V3", 15, 15, 1.6, 1.6)])
    pad = b.FindFootprintByReference("C9").Pads()[0]
    spots = br.via_spots(br.Space(b), pad, "+3V3")
    assert spots
    for p in spots:
        assert not pad.HitTest(v(*p))
        d = min(br.seg_poly(p, p, poly) for poly in br.pad_poly(pad, F))
        assert d >= route.PLANE_VIA_MIN_MM - 1e-6, (p, d)


def test_every_via_on_the_carrier_copy_keeps_0_4mm_from_every_pad(carrier):
    """P28 on the whole copy (fanout, moves, rivers): every via centre at least 0.4 mm from every
    SMD pad's copper (P19's spacing for its own pad; more than the 0.45 mm clearance to others)."""
    path, _, _ = carrier
    b = pcbnew.LoadBoard(str(path))
    pads = [(f"{fp.GetReference()}.{p.GetNumber()}", poly) for fp in b.GetFootprints()
            for p in fp.Pads() if p.GetAttribute() == pcbnew.PAD_ATTRIB_SMD
            for L in (F, B) if p.IsOnLayer(L) for poly in br.pad_poly(p, L)]
    worst = (9.0, "")
    for t in b.GetTracks():
        if t.GetClass() == "PCB_VIA":
            c = br._pt(t.GetPosition())
            for label, poly in pads:
                d = 0.0 if br.inside(c, poly) else br.seg_poly(c, c, poly)
                worst = min(worst, (d, f"{t.GetNetname()} via {c} to {label}"))
    assert worst[0] >= route.PLANE_VIA_MIN_MM - 0.01, worst     # 0.01: rounded pads as polygons


def tab_board():
    """A 30 x 20 mm body with a 14 x 7 mm finger tab below it (x 8-22), as the card's."""
    b = make(w=30, h=20)
    for item in list(b.GetDrawings()):
        b.Remove(item)
    pts = [(0, 0), (30, 0), (30, 20), (22, 20), (22, 27), (8, 27), (8, 20), (0, 20)]
    for a, c in zip(pts, pts[1:] + pts[:1]):
        s = pcbnew.PCB_SHAPE(b)
        s.SetShape(pcbnew.SHAPE_T_SEGMENT)
        s.SetLayer(pcbnew.Edge_Cuts)
        s.SetWidth(pcbnew.FromMM(0.1))
        s.SetStart(v(*a))
        s.SetEnd(v(*c))
        b.Add(s)
    part(b, "J1", [(1, "N0", 15, 24, 1.8, 5.0)])
    part(b, "U1", [(1, "N0", 15, 8, 0.6, 0.6)])
    return b


def test_bus_route_puts_no_via_in_the_finger_tab():
    b = tab_board()
    r = br.Router(b)
    hard, _ = r.space.check("N0", br.Via((15.0, 21.5)))      # below the body: in the tab
    assert any("tab" in h for h in hard), hard
    assert not r.space.check("N0", br.Via((15.0, 18.5)))[0]  # above it: fine
    pen = br.Pen((15, 24), F, 0.15).vert(21.5).via(B).vert(8.0)
    assert r.offer("N0", "test", pen.items) is False
    assert "tab" in r.report.left["N0"]


# --- P29: the power paths -------------------------------------------------------------------

def _pieces(b, net):
    return len(route._components(b, b.FindNet(net).GetNetCode()))


def test_5v_mod_is_laid_whole_along_the_edges_and_the_highway(carrier):
    """+5V_MOD from D1 up the right edge, across the highway under the module, to J2's 5V pins
    and down the left edge to VR1 and its capacitors: whole, locked, at the Power width with
    necks, and never in the band between J2 and the translators."""
    path, report, _ = carrier
    b = pcbnew.LoadBoard(str(path))
    assert _pieces(b, "+5V_MOD") == 1, report.left
    mine = [t for t in b.GetTracks() if t.GetNetname() == "+5V_MOD"]
    assert mine and all(t.IsLocked() for t in mine)
    for t in mine:
        for x, y in (br._pt(t.GetStart()), br._pt(t.GetEnd())):
            assert not (8.0 < x < 73.5 and 27.5 < y < 50.0), (x, y)     # the band stays free
    assert [p for p in check_drc.problems_width(b, path.with_suffix(".kicad_pro"))
            if "+5V_MOD" in p] == []


def test_5v_reaches_u2_by_vias_under_its_body_and_is_laid_or_reported(carrier):
    """+5V: U2's VCCB pins 7 and 18 by vias under U2's body joined on B.Cu, their capacitors
    by necks, U3's pin by its capacitor, D1 and C2 together; the joins between those groups are
    Freerouting's, and listed as left. Every piece is laid or listed."""
    path, report, _ = carrier
    b = pcbnew.LoadBoard(str(path))
    u2 = b.FindFootprintByReference("U2")
    body = [p for p in u2.Pads() if p.GetNetname() == "+5V"]
    assert len(body) == 2
    vias = [t for t in b.GetTracks() if t.GetClass() == "PCB_VIA" and t.GetNetname() == "+5V"]
    lo = min(pcbnew.ToMM(p.GetY()) for p in u2.Pads())
    hi = max(pcbnew.ToMM(p.GetY()) for p in u2.Pads())
    for p in body:                            # a via straight in from the pin, under the body
        assert any(abs(pcbnew.ToMM(v_.GetX() - p.GetX())) < 0.01
                   and lo < pcbnew.ToMM(v_.GetY()) < hi for v_ in vias), p.GetNumber()
    assert any(t.GetLayer() == B and t.GetNetname() == "+5V" for t in b.GetTracks()
               if t.GetClass() != "PCB_VIA")
    assert _pieces(b, "+5V") == 1 or any(k.startswith("+5V") and "+5V_MOD" not in k
                                         for k in report.left), report.left
    assert [p for p in check_drc.problems_width(b, path.with_suffix(".kicad_pro"))
            if "+5V " in p] == []


def test_the_power_paths_are_in_the_report(carrier):
    _, report, _ = carrier
    laid = report.laid.get("power", [])
    assert any(s.startswith("+5V_MOD") for s in laid) and any(s.startswith("+5V ") for s in laid)


def test_bus_route_still_lays_the_committed_boards_locked_copper(carrier):
    """The drift test: the copper bus_route lays on a fresh copy (after the fanout) is, line
    for line, in the committed board's locked copper. What the board has beyond it belongs to
    nets bus_route did not lay (the clocks, the first pass). So plan_buses,
    bus_route and the fanout still produce what is committed; a changed lane spacing, say,
    fails here in a minute instead of at `repro`."""
    path, report, _ = carrier

    def locked(p):
        return {br.signature_line(t) for t in pcbnew.LoadBoard(str(p)).GetTracks()
                if t.IsLocked()}

    mine, committed = locked(path), locked(CARRIER)
    assert mine
    assert mine <= committed, sorted(mine - committed)[:5]
    beyond = {line.split()[1] for line in committed - mine}
    laid = {n for nets in report.laid.values() for n in nets}
    assert not beyond & laid, sorted(beyond & laid)
