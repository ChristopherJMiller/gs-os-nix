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
