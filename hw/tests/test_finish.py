# SPDX-License-Identifier: GPL-3.0-or-later
"""route.finish_unrouted on small hand-made boards (24 x 12 mm, SMD pads on the front):
which pieces it joins, that nothing is left dangling, that a rip-up puts back what it
took when it fails, and that a rail is laid at its net class width and never thinner."""
from pathlib import Path

import pcbnew
import pytest

from hw.tools import route

ROOT = Path(__file__).resolve().parents[2]
F, B = pcbnew.F_Cu, pcbnew.B_Cu
W, H = 24.0, 12.0


def v(x, y):
    return pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y))


def make_board(tmp_path, pads, tracks=(), vias=()):
    """pads: {net: [(x, y)]}; tracks: [(net, layer, (x0, y0), (x1, y1), width)]; vias:
    [(net, (x, y))]. +5V is in a Power class of 0.5 mm, as in carrier.kicad_pro."""
    b = pcbnew.BOARD()
    for a, c in [((0, 0), (W, 0)), ((W, 0), (W, H)), ((W, H), (0, H)), ((0, H), (0, 0))]:
        s = pcbnew.PCB_SHAPE(b)
        s.SetShape(pcbnew.SHAPE_T_SEGMENT)
        s.SetLayer(pcbnew.Edge_Cuts)
        s.SetWidth(pcbnew.FromMM(0.1))
        s.SetStart(v(*a))
        s.SetEnd(v(*c))
        b.Add(s)
    names = set(pads) | {n for n, *_ in tracks} | {n for n, _ in vias} | {"GND"}
    for n in sorted(names):
        b.Add(pcbnew.NETINFO_ITEM(b, n))
    k = 0
    for n, pts in pads.items():
        for x, y in pts:
            k += 1
            fp = pcbnew.FOOTPRINT(b)
            fp.SetReference(f"P{k}")
            pad = pcbnew.PAD(fp)
            pad.SetNumber("1")
            pad.SetShape(pcbnew.PAD_SHAPE_RECT)
            pad.SetAttribute(pcbnew.PAD_ATTRIB_SMD)
            pad.SetLayerSet(pad.SMDMask())
            pad.SetSize(v(0.8, 0.8))
            pad.SetPosition(v(x, y))
            pad.SetNet(b.FindNet(n))
            fp.Add(pad)
            b.Add(fp)
    for n, layer, a, c, w in tracks:
        t = pcbnew.PCB_TRACK(b)
        t.SetStart(v(*a))
        t.SetEnd(v(*c))
        t.SetLayer(layer)
        t.SetWidth(pcbnew.FromMM(w))
        t.SetNet(b.FindNet(n))
        b.Add(t)
    for n, p in vias:
        x = pcbnew.PCB_VIA(b)
        x.SetPosition(v(*p))
        x.SetViaType(pcbnew.VIATYPE_THROUGH)
        x.SetLayerPair(F, B)
        x.SetWidth(pcbnew.FromMM(0.6))
        x.SetDrill(pcbnew.FromMM(0.3))
        x.SetNet(b.FindNet(n))
        b.Add(x)
    ns = b.GetDesignSettings().m_NetSettings
    power = pcbnew.NETCLASS("Power")                  # as in carrier.kicad_pro
    power.SetTrackWidth(pcbnew.FromMM(0.5))
    ns.SetNetclass("Power", power)
    ns.SetNetclassPatternAssignment("+5V", "Power")
    default = ns.GetDefaultNetclass()
    default.SetTrackWidth(pcbnew.FromMM(0.15))
    default.SetClearance(pcbnew.FromMM(0.15))
    ns.RecomputeEffectiveNetclasses()
    return b


def code(b, name):
    return b.FindNet(name).GetNetCode()


def pieces(b, name):
    return len(route._components(b, code(b, name)))


def snapshot(b):
    return sorted((t.GetClass(), t.GetNetname(), t.GetStart().x, t.GetStart().y,
                   t.GetEnd().x, t.GetEnd().y, t.GetLayer(), t.GetWidth() if t.GetClass() !=
                   "PCB_VIA" else 0) for t in b.GetTracks())


def dangling(b):
    """Track ends that touch nothing of their net: no pad, no via, no other track end."""
    out = []
    tracks = [t for t in b.GetTracks() if t.GetClass() != "PCB_VIA"]
    pads = [p for fp in b.GetFootprints() for p in fp.Pads()]
    others = [t for t in b.GetTracks()]
    for t in tracks:
        for pt in (t.GetStart(), t.GetEnd()):
            ok = any(p.GetNetCode() == t.GetNetCode() and p.HitTest(pt) for p in pads)
            ok = ok or any(o is not t and o.GetNetCode() == t.GetNetCode() and
                           (o.GetClass() == "PCB_VIA" and (o.GetPosition() - pt).EuclideanNorm()
                            < 1000 or o.GetClass() != "PCB_VIA" and o.GetLayer() == t.GetLayer()
                            and o.HitTest(pt)) for o in others)
            if not ok:
                out.append((t.GetNetname(), pcbnew.ToMM(pt.x), pcbnew.ToMM(pt.y)))
    return out


def wall_board(tmp_path):
    """N1 wants to cross x = 12, where N2 runs top to bottom on both layers (vias at the
    ends join its two tracks); N2 can go round by the back layer once N1 is across."""
    return make_board(
        tmp_path,
        {"N1": [(10.5, 6), (21, 6)], "N2": [(12, 1.0), (12, 11.0)], "GND": [(22, 2)]},
        tracks=[("N2", F, (12, 1.0), (12, 11.0), 0.2), ("N2", B, (12, 1.0), (12, 11.0), 0.2)],
        vias=[("N2", (12, 1.0)), ("N2", (12, 11.0))])


def test_components_follow_copper_that_touches(tmp_path):
    b = make_board(tmp_path, {"N1": [(3, 6), (21, 6)], "GND": [(22, 2)]},
                   tracks=[("N1", F, (3, 6), (10, 6), 0.15)])
    assert pieces(b, "N1") == 2
    t = pcbnew.PCB_TRACK(b)
    t.SetStart(v(10, 6))
    t.SetEnd(v(21, 6))
    t.SetLayer(F)
    t.SetWidth(pcbnew.FromMM(0.15))
    t.SetNet(b.FindNet("N1"))
    b.Add(t)
    assert pieces(b, "N1") == 1                       # the two tracks meet end to end
    t.SetLayer(B)
    assert pieces(b, "N1") == 3          # on the back it touches neither track nor pad


def test_join_pieces_joins_two_pads_and_leaves_nothing_dangling(tmp_path):
    b = make_board(tmp_path, {"N1": [(3, 6), (21, 6)], "GND": [(22, 2)]})
    made = []
    assert route._join_pieces(b, code(b, "N1"), 0.15, made)
    assert made and pieces(b, "N1") == 1
    assert dangling(b) == []
    assert all(t.GetNetname() == "N1" for t in b.GetTracks())


def test_a_net_walled_in_cannot_be_joined_plainly(tmp_path):
    b = wall_board(tmp_path)
    before = snapshot(b)
    assert not route._join_pieces(b, code(b, "N1"), 0.15, [])
    assert snapshot(b) == before                      # and it left nothing behind


def test_rip_up_moves_the_small_net_and_both_are_whole(tmp_path):
    b = wall_board(tmp_path)
    assert route._rip_up_and_join(b, code(b, "N1"), 0.15) == "N2"
    assert pieces(b, "N1") == 1 and pieces(b, "N2") == 1
    assert dangling(b) == []
    n1 = [t for t in b.GetTracks() if t.GetNetname() == "N1"]
    n2 = [t for t in b.GetTracks() if t.GetNetname() == "N2"]
    assert not any(route._clashes(t, n1) for t in n2)  # it kept its clearance


def test_a_failed_rip_up_puts_everything_back(tmp_path, monkeypatch):
    b = wall_board(tmp_path)
    before = snapshot(b)
    n2 = code(b, "N2")
    real = route._join_pieces

    def join(board, net, width, made, ignore=()):
        return False if net == n2 else real(board, net, width, made, ignore)

    monkeypatch.setattr(route, "_join_pieces", join)  # N2 cannot be routed again
    assert route._rip_up_and_join(b, code(b, "N1"), 0.15) is None
    assert snapshot(b) == before
    assert pieces(b, "N1") == 2 and pieces(b, "N2") == 1


def test_a_net_of_many_pads_is_not_taken_apart(tmp_path):
    b = make_board(
        tmp_path,
        {"N1": [(10.5, 6), (21, 6)], "N2": [(12, 1.0), (12, 11.0), (1, 1), (3, 1), (5, 1), (7, 1)],
         "GND": [(22, 2)]},
        tracks=[("N2", F, (12, 1.0), (12, 11.0), 0.2), ("N2", B, (12, 1.0), (12, 11.0), 0.2)],
        vias=[("N2", (12, 1.0)), ("N2", (12, 11.0))])
    before = snapshot(b)
    assert route._rip_up_and_join(b, code(b, "N1"), 0.15) is None
    assert snapshot(b) == before


# --- widths from the net class (R23) ---------------------------------------------------------

def test_the_width_of_a_net_is_its_classs(tmp_path):
    b = make_board(tmp_path, {"+5V": [(3, 3), (21, 3)], "N1": [(3, 9), (21, 9)]})
    assert route.net_width_mm(b, "+5V") == pytest.approx(0.5)
    assert route.net_width_mm(b, "N1") == pytest.approx(0.15)


def test_a_rail_is_joined_at_its_class_width_and_a_signal_at_the_minimum(tmp_path):
    b = make_board(tmp_path, {"+5V": [(3, 3), (21, 3)], "N1": [(3, 9), (21, 9)],
                              "GND": [(22, 6)]})
    assert route.finish_unrouted(b) == []
    width = {n: {t.GetWidth() for t in b.GetTracks() if t.GetNetname() == n} for n in
             ("+5V", "N1")}
    assert width["+5V"] == {pcbnew.FromMM(0.5)}
    assert width["N1"] == {pcbnew.FromMM(0.15)}


def gap_board(tmp_path):
    """+5V across a wall of N2 (six pads, so not a net to take apart) that has a 0.7 mm
    gap in it, on both layers: room for a 0.15 mm track, not for a 0.5 mm one."""
    wall = [(F, (12, 0.6), (12, 5.55)), (F, (12, 6.45), (12, 11.4)),
            (B, (12, 0.6), (12, 5.55)), (B, (12, 6.45), (12, 11.4))]
    return make_board(
        tmp_path,
        {"+5V": [(3, 6), (21, 6)], "N2": [(1, 1), (3, 1), (5, 1), (7, 1), (9, 1), (10.5, 1)],
         "GND": [(22, 2)]},
        tracks=[("N2", layer, a, c, 0.2) for layer, a, c in wall])


def test_a_rail_that_does_not_fit_at_its_width_is_reported_not_thinned(tmp_path):
    b = gap_board(tmp_path)
    left = route.finish_unrouted(b)
    assert any(m.startswith("+5V") and "0.50" in m for m in left)
    thin = [t for t in b.GetTracks()
            if t.GetNetname() == "+5V" and t.GetClass() != "PCB_VIA"
            and t.GetWidth() < pcbnew.FromMM(0.5)]
    assert thin == []


def test_the_same_gap_passes_a_signal_width_track(tmp_path):
    b = gap_board(tmp_path)
    assert route._join_pieces(b, code(b, "+5V"), 0.15, [])   # proves the gap was the limit
