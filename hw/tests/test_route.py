# SPDX-License-Identifier: GPL-3.0-or-later
"""route.py's DSN preparation: case-twin nets, the smd_smd rule, the edge keepout strips;
and check_drc's parity half."""
from pathlib import Path

import numpy as np
import pcbnew
import pytest

from hw.tools import build_board as bb
from hw.tools import check_drc as cd
from hw.tools import netlist as nlmod
from hw.tools import route

FIX = Path(__file__).parent / "fixtures"

DSN = """(pcb "/tmp/x.dsn"
  (parser
    (string_quote ")
    (space_in_quoted_tokens on)
  )
  (structure
    (boundary
      (path pcb 0  8000 -6000  7000 -6000  7000 -7000  1000 -7000
            1000 -6000  0 -6000  0 0  8000 0  8000 -6000)
    )
    (via "Via[0-1]_600:300_um")
    (rule
      (width 150)
      (clearance 150)
      (clearance 37.5 (type smd_smd))
    )
  )
  (network
    (net SLOT_D3
      (pins U1-1)
    )
    (net slot_d3
      (pins U2-1)
    )
    (net GND
      (pins U2-2)
    )
    (class kicad_default SLOT_D3 slot_d3 GND
      (rule (width 150))
    )
  )
)
"""


def test_case_twins_are_renamed_and_restored():
    out, back = route.prepare_dsn(DSN)
    assert back == {"slot_d3_dup1": "slot_d3"}
    assert "(net slot_d3_dup1" in out and "(net SLOT_D3" in out
    assert "SLOT_D3 slot_d3_dup1" in out
    assert route.restore_ses("(net slot_d3_dup1 (wire))", back) == "(net slot_d3 (wire))"


def test_planes_are_dropped_and_nets_kept():
    dsn = DSN.replace("(boundary", '(plane GND (polygon F.Cu 0  0 0  10 0  10 -10))\n    (boundary')
    out, _ = route.prepare_dsn(dsn)
    assert "(plane" not in out and "(net GND" in out


def test_smd_smd_rule_is_dropped():
    out, _ = route.prepare_dsn(DSN)
    assert "smd_smd" not in out and "(clearance 170)" in out   # 0.15 mm + margin


def test_edge_strips_cover_every_edge_but_the_tab_bottom():
    out, _ = route.prepare_dsn(DSN)
    strips = [l for l in out.splitlines() if 'keepout "" (polygon F.Cu' in l]
    assert len(strips) == 7                 # 8 edges less the tab's bottom
    pts = route.boundary_points(DSN)
    for poly in route.edge_strips(pts, 350):
        ys = [y for _, y in poly]
        xs = [x for x, _ in poly]
        assert min(xs) < max(xs) and min(ys) < max(ys)
    # the body's top edge (y = 0): its strip lies inside the board, at y -350 .. 0
    top = [p for p in route.edge_strips(pts, 350) if all(y in (0, -350) for _, y in p)]
    assert len(top) == 1


def test_case_collision_with_a_suffixed_name_is_refused():
    dsn = DSN.replace("(net GND", "(net slot_d3_dup1\n      (pins U3-1)\n    )\n    (net GND")
    with pytest.raises(SystemExit):
        route.prepare_dsn(dsn)


class _Maze:
    """Just enough of route._Grid for route._astar: a 40 x 20 cell board."""
    w, h = 40, 20

    def __init__(self):
        self.track = {pcbnew.F_Cu: np.zeros((self.h, self.w), bool),
                      pcbnew.B_Cu: np.zeros((self.h, self.w), bool)}
        self.via = np.zeros((self.h, self.w), bool)


def _mask(cells):
    m = np.zeros((_Maze.h, _Maze.w), bool)
    for i, j in cells:
        m[j, i] = True
    return {pcbnew.F_Cu: m, pcbnew.B_Cu: np.zeros_like(m)}


def test_astar_goes_round_a_wall_and_over_it_with_a_via():
    g = _Maze()
    g.track[pcbnew.F_Cu][:, 20] = True                 # a wall across the front layer...
    g.track[pcbnew.F_Cu][0, 20] = False                # ...with a gap at the top
    path = route._astar(g, _mask([(5, 10)]), _mask([(35, 10)]))
    assert path[0][1:] == (5, 10) and path[-1][1:] == (35, 10)
    assert {l for l, _, _ in path} == {pcbnew.F_Cu}
    g.track[pcbnew.F_Cu][0, 20] = True                 # no gap: the back layer it is
    path = route._astar(g, _mask([(5, 10)]), _mask([(35, 10)]))
    assert {l for l, _, _ in path} == {pcbnew.F_Cu, pcbnew.B_Cu}
    g.track[pcbnew.B_Cu][:, 20] = True                 # walled on both layers: none
    assert route._astar(g, _mask([(5, 10)]), _mask([(35, 10)])) is None


def test_a_path_is_laid_as_tracks_and_vias(tmp_path):
    out = tmp_path / "tiny.kicad_pcb"
    bb.build(FIX / "tiny.net", FIX / "tiny_placement.toml", out, update=False)
    board = pcbnew.LoadBoard(str(out))
    net = next(n.GetNetCode() for n in board.GetNetsByName().values()
               if n.GetNetCode() and n.GetNetname() != "GND")
    grid = _Maze()
    grid.x0 = grid.y0 = 0
    grid.centre = lambda i, j: pcbnew.VECTOR2I(int((i + 0.5) * route.RES_MM * 1e6),
                                               int((j + 0.5) * route.RES_MM * 1e6))
    F, B = pcbnew.F_Cu, pcbnew.B_Cu
    path = [(F, 1, 1), (F, 2, 1), (F, 3, 1), (F, 4, 2), (B, 4, 2), (B, 4, 3), (B, 4, 4)]
    made = route._lay_path(board, grid, net, path, 0.2)
    assert [m.GetClass() for m in made] == ["PCB_TRACK", "PCB_TRACK", "PCB_VIA", "PCB_TRACK"]
    assert all(m.GetNetCode() == net for m in made)
    assert made[0].GetWidth() == pcbnew.FromMM(0.2)


def test_parity_finds_a_pad_on_the_wrong_net(tmp_path):
    out = tmp_path / "tiny.kicad_pcb"
    bb.build(FIX / "tiny.net", FIX / "tiny_placement.toml", out, update=False)
    nl = nlmod.load(FIX / "tiny.net")
    board = pcbnew.LoadBoard(str(out))
    assert cd.problems_parity(board, nl) == []
    pad = next(iter(next(iter(board.GetFootprints())).Pads()))
    other = next(n for n in board.GetNetsByName().values()
                 if n.GetNetCode() not in (0, pad.GetNetCode()))
    pad.SetNet(other)
    assert any("parity:" in p and "netlist says" in p for p in cd.problems_parity(board, nl))


# --- the first pass: clocks and the data bus, laid by the finisher, locked, kept by the router

def _first_board(tmp_path):
    """A 24 x 12 mm board: a clock CLK across the middle, a data net D1 above it, and X
    for the router to do (below). CLK is in a Clock class of 0.2 mm / 0.4 mm clearance."""
    from hw.tests.test_finish import make_board
    b = make_board(tmp_path, {"CLK": [(3, 6), (21, 6)], "D1": [(3, 3), (21, 3)],
                              "X": [(3, 9), (21, 9)], "GND": [(22, 11)]})
    ns = b.GetDesignSettings().m_NetSettings
    clock = pcbnew.NETCLASS("Clock")
    clock.SetTrackWidth(pcbnew.FromMM(0.2))
    clock.SetClearance(pcbnew.FromMM(0.4))
    ns.SetNetclass("Clock", clock)
    ns.SetNetclassPatternAssignment("CLK", "Clock")
    ns.RecomputeEffectiveNetclasses()
    return b


def _ses(net, y_um):
    return f"""(session x.ses
  (base_design x.dsn)
  (placement (resolution um 1))
  (was_is)
  (routes
    (resolution um 1)
    (parser (host_cad "KiCad's Pcbnew") (host_version "x"))
    (library_out)
    (network_out
      (net {net} (wire (path F.Cu 150  3000 {y_um}  21000 {y_um})))
    )
  )
)
"""


def test_first_nets_are_routed_first_and_locked(tmp_path):
    board = _first_board(tmp_path)
    path = tmp_path / "first.kicad_pcb"
    pcbnew.SaveBoard(str(path), board)
    seen = {}

    def stub(dsn, ses, passes, timeout_s, log, extra=()):
        """Freerouting, stood in for: record what it was given; route X, and nothing else."""
        text = Path(dsn).read_text()
        seen["wires"] = [l.strip() for l in text.splitlines() if l.strip().startswith("(wire")]
        seen["clock_class"] = "(width 200)" in text          # the Clock class reached the DSN
        # ...and its 0.4 mm is given back as fat fixed wires (0.2 + 2 * (0.4 - 0.17) mm)
        seen["halo"] = any("(path F.Cu 660 " in w and "(net CLK)" in w for w in seen["wires"])
        Path(ses).write_text(_ses("X", -9000))
        return 0

    route.route(path, first=["CLK", "D1"], router=stub)
    b = pcbnew.LoadBoard(str(path))
    by = {}
    for t in b.GetTracks():
        by.setdefault(t.GetNetname(), []).append(t)
    # laid before the router ran, locked, and handed to it as fixed wires
    assert set(by) == {"CLK", "D1", "X"}
    for name, width in (("CLK", 0.2), ("D1", 0.15)):
        assert all(t.IsLocked() for t in by[name]), name
        assert all(t.GetWidth() == pcbnew.FromMM(width) for t in by[name]), name
        assert any(f"(net {name})(type fix)" in w for w in seen["wires"]), seen["wires"]
    assert not any("(net X)" in w for w in seen["wires"])        # X was the router's to do
    assert seen["clock_class"] and seen["halo"]
    assert not any("(path F.Cu 660 " in w and "(net D1)" in w for w in seen["wires"])
    # the router's pass did not move them: still the straight shortest run between the pads
    for name, y in (("CLK", 6.0), ("D1", 3.0)):
        ts = by[name]
        assert len(ts) == 1 and ts[0].GetClass() == "PCB_TRACK"
        ys = {pcbnew.ToMM(ts[0].GetStart().y), pcbnew.ToMM(ts[0].GetEnd().y)}
        assert len(ys) == 1 and abs(ys.pop() - y) <= 0.4          # straight, and on the pads
    assert not any(t.IsLocked() for t in by["X"])                 # the rest stays the router's
    # and the router's work is joined: nothing left over
    assert route.finish_unrouted(b) == []


def test_drop_net_removes_the_net_and_its_class_entry_but_not_its_pins():
    out = route.drop_net(DSN, "GND")
    assert "(net GND" not in out and "GND" not in out.split("(class")[1]
    assert "U2-2" not in out.split("(network")[1].split("(class")[0]    # the net block is gone
    assert "(net SLOT_D3" in out and "(net slot_d3" in out              # the others stay
    assert out.count("(") == out.count(")")
