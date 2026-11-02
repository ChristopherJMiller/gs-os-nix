# SPDX-License-Identifier: GPL-3.0-or-later
"""route.py's DSN preparation: case-twin nets, the smd_smd rule, the edge keepout strips;
and check_drc's parity half."""
import os
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


def test_planes_stay_in_the_dsn_and_nets_are_kept():
    """P20: In1/In2 are planes to the router too, so it knows the plane nets' pins are joined
    by them (the fanout vias are what put them on the plane)."""
    plane = "(plane GND (polygon In1.Cu 0  0 0  10 0  10 -10))"
    dsn = DSN.replace("(boundary", plane + "\n    (boundary")
    out, _ = route.prepare_dsn(dsn)
    assert plane in out and "(net GND" in out


def test_smd_smd_rule_is_dropped():
    out, _ = route.prepare_dsn(DSN)
    assert "smd_smd" not in out and "(clearance 170)" in out   # 0.15 mm + margin


def test_edge_strips_cover_every_edge_but_the_tab_bottom():
    out, _ = route.prepare_dsn(DSN)
    strips = [l for l in out.splitlines() if l.strip().startswith('(keepout "" (polygon F.Cu')]
    assert len(strips) == 7                 # 8 edges less the tab's bottom
    pts = route.boundary_points(DSN)
    for poly in route.edge_strips(pts, 350):
        ys = [y for _, y in poly]
        xs = [x for x, _ in poly]
        assert min(xs) < max(xs) and min(ys) < max(ys)
    # the body's top edge (y = 0): its strip lies inside the board, at y -350 .. 0
    top = [p for p in route.edge_strips(pts, 350) if all(y in (0, -350) for _, y in p)]
    assert len(top) == 1


def test_the_router_gets_a_via_keepout_over_the_tab():
    """hw-edge forbids a via whose inner pad reaches below the tab's top edge (y 6000 up here,
    the tab x 1000-7000 down to y -7000); Freerouting put four there on the carrier. The DSN
    keeps vias out of the tab and TAB_VIA_MM above it, on both layers."""
    out, _ = route.prepare_dsn(DSN)
    top = -6000 + int(route.TAB_VIA_MM * 1000)
    for layer in ("F.Cu", "B.Cu"):
        want = (f'(via_keepout "" (polygon {layer} 0  1000 -7000  7000 -7000  7000 {top}  '
                f'1000 {top}  1000 -7000))')
        assert want in out, [l for l in out.splitlines() if "via_keepout" in l]


def test_the_finisher_puts_no_via_where_its_pad_reaches_the_tab(tmp_path):
    """The A* grid refuses a via whose 0.6 mm pad comes within TAB_VIA_MM of the tab's top edge
    (hw-edge: no inner copper below it), and allows one clear of it."""
    from hw.tests.test_bus_route import tab_board          # body 30 x 20, tab x 8-22 below y 20
    g = route._Grid(tab_board(), 0, 0.15)

    def blocked(x, y):
        i, j = g.px(pcbnew.FromMM(x), pcbnew.FromMM(y))
        return bool(g.via[int(j), int(i)])

    edge = 20.0 - route.VIA_MM / 2 - route.TAB_VIA_MM
    assert blocked(15, edge + 0.06) and blocked(15, 21.5)
    assert not blocked(15, edge - 0.1)


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
    for the router to do (below). CLK is in a Clock class of 0.2 mm / 0.3 mm clearance."""
    from hw.tests.test_finish import make_board
    b = make_board(tmp_path, {"CLK": [(3, 6), (21, 6)], "D1": [(3, 3), (21, 3)],
                              "X": [(3, 9), (21, 9)], "GND": [(22, 11)]})
    ns = b.GetDesignSettings().m_NetSettings
    clock = pcbnew.NETCLASS("Clock")
    clock.SetTrackWidth(pcbnew.FromMM(0.2))
    clock.SetClearance(pcbnew.FromMM(0.3))
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
        # ...and its 0.3 mm is given back as fat fixed wires (0.2 + 2 * (0.3 - 0.17) mm)
        seen["halo"] = any("(path F.Cu 460 " in w and "(net CLK)" in w for w in seen["wires"])
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
    assert not any("(path F.Cu 460 " in w and "(net D1)" in w for w in seen["wires"])
    # the router's pass did not move them: still the straight shortest run between the pads
    for name, y in (("CLK", 6.0), ("D1", 3.0)):
        ts = by[name]
        assert len(ts) == 1 and ts[0].GetClass() == "PCB_TRACK"
        ys = {pcbnew.ToMM(ts[0].GetStart().y), pcbnew.ToMM(ts[0].GetEnd().y)}
        assert len(ys) == 1 and abs(ys.pop() - y) <= 0.4          # straight, and on the pads
    assert not any(t.IsLocked() for t in by["X"])                 # the rest stays the router's
    # and the router's work is joined: nothing left over
    assert route.finish_unrouted(b) == []


def test_gnd_stays_in_the_dsn_as_a_routed_net():
    """P15 (plan 1's lesson): GND's pins stay in the DSN's network. With the In1.Cu plane the
    router fans them out to it by vias instead of laying tracks, but it has to be told which."""
    out, _ = route.prepare_dsn(DSN)
    assert "(net GND" in out and "U2-2" in out
    assert not hasattr(route, "drop_net")


# --- the repair passes: clock clearance --------------------------------

def _track(b, net, layer, a, c, w=0.15):
    from hw.tests.test_finish import v
    t = pcbnew.PCB_TRACK(b)
    t.SetStart(v(*a))
    t.SetEnd(v(*c))
    t.SetLayer(layer)
    t.SetWidth(pcbnew.FromMM(w))
    t.SetNet(b.FindNet(net))
    b.Add(t)
    return t


def _clock_board(tmp_path):
    """CLK runs along y = 3 (Clock class: 0.2 mm wide, 0.3 mm clearance); N's pads are at
    y = 5 and its track climbs to y = 3.4 and runs beside the clock, 0.225 mm from it."""
    from hw.tests.test_finish import make_board
    b = make_board(tmp_path, {"CLK": [(3, 3), (21, 3)], "N": [(3, 5), (21, 5)], "GND": [(22, 11)]})
    ns = b.GetDesignSettings().m_NetSettings
    clock = pcbnew.NETCLASS("Clock")
    clock.SetTrackWidth(pcbnew.FromMM(0.2))
    clock.SetClearance(pcbnew.FromMM(0.3))
    ns.SetNetclass("Clock", clock)
    ns.SetNetclassPatternAssignment("CLK", "Clock")
    ns.RecomputeEffectiveNetclasses()
    F = pcbnew.F_Cu
    _track(b, "CLK", F, (3, 3), (21, 3), 0.2)
    for a, c in (((3, 5), (3, 3.4)), ((3, 3.4), (21, 3.4)), ((21, 3.4), (21, 5))):
        _track(b, "N", F, a, c)
    return b


def test_clock_violations_finds_a_track_too_near_a_clock(tmp_path):
    b = _clock_board(tmp_path)
    assert route.clock_violations(b) == {"N"}
    assert route.clock_violations(_first_board(tmp_path)) == set()      # nothing laid: none


def test_repair_clock_clearance_relays_the_offender_clear_of_the_clock(tmp_path):
    b = _clock_board(tmp_path)
    assert route.repair_clock_clearance(b) == []
    assert route.clock_violations(b) == set()
    assert len(route._components(b, b.FindNet("N").GetNetCode())) == 1   # still one piece
    clk = [t for t in b.GetTracks() if t.GetNetname() == "CLK"]
    assert len(clk) == 1 and clk[0].GetWidth() == pcbnew.FromMM(0.2)     # the clock is untouched


def test_repair_leaves_locked_tracks_alone(tmp_path):
    b = _clock_board(tmp_path)
    for t in b.GetTracks():
        t.SetLocked(True)
    assert route.repair_clock_clearance(b) == ["N"]                      # reported, not moved
    assert route.clock_violations(b) == {"N"}


def test_prune_dangling_takes_a_spur_and_keeps_a_joined_track(tmp_path):
    from hw.tests.test_finish import make_board
    b = make_board(tmp_path, {"N": [(3, 6), (21, 6)], "GND": [(22, 11)]})
    F = pcbnew.F_Cu
    _track(b, "N", F, (3, 6), (21, 6))                  # joins the two pads
    _track(b, "N", F, (10, 6), (10, 9))                 # leaves the track and ends on nothing
    assert route.prune_dangling(b) == 1
    assert len(list(b.GetTracks())) == 1


def test_a_router_stub_inside_its_own_pad_is_dropped(tmp_path):
    """Freerouting joins a fixed wire that ends inside a pad to the pad's centre: on the carrier
    a 2.77 mm track inside every J1 finger, nothing electrically, but hw-load counts its length.
    Unlocked tracks wholly inside a pad of their net go; a track that leaves the pad, a locked
    one, and one in a pad of another net stay."""
    from hw.tests.test_finish import make_board
    b = make_board(tmp_path, {"N": [(4, 6), (20, 6)], "M": [(12, 6)], "GND": [(22, 11)]})
    for fp in b.GetFootprints():
        if fp.Pads()[0].GetNetname() == "N":
            fp.Pads()[0].SetSize(pcbnew.VECTOR2I(pcbnew.FromMM(1.8), pcbnew.FromMM(5.0)))
    F = pcbnew.F_Cu
    _track(b, "N", F, (4, 4.0), (20, 4.0))              # the fixed wire, ends 2 mm above centre
    for t in b.GetTracks():
        t.SetLocked(True)
    _track(b, "N", F, (4, 4.0), (4, 6.0))               # the router's stub: inside pad 1
    _track(b, "N", F, (20, 4.3), (20, 6.0))             # inside pad 2 too
    _track(b, "N", F, (4, 7.0), (6, 9.0))               # leaves the pad: stays
    _track(b, "M", F, (12, 5.8), (12, 6.2))             # inside M's own pad: dropped too
    _track(b, "N", F, (11.9, 6.0), (12.1, 6.0))         # inside a pad of another net: stays
    assert route.drop_pad_stubs(b) == 3
    left = sorted((t.GetNetname(), pcbnew.ToMM(t.GetStart().x), pcbnew.ToMM(t.GetStart().y))
                  for t in b.GetTracks())
    assert left == [("N", 4.0, 4.0), ("N", 4.0, 7.0), ("N", 11.9, 6.0)]
    pieces = route._components(b, b.FindNet("N").GetNetCode())
    assert max(len(p["pad"]) for p in pieces) == 2                 # the pads are still joined


def test_route_drops_the_router_stubs_inside_their_pads(tmp_path):
    from hw.tests.test_finish import make_board
    path = tmp_path / "s.kicad_pcb"
    pcbnew.SaveBoard(str(path),
                     make_board(tmp_path, {"X": [(3, 6), (21, 6)], "GND": [(22, 11)]}))

    def stub(dsn, ses, passes, timeout_s, log, extra=()):
        wire = "(wire (path F.Cu 150  3000 -6000  21000 -6000))"
        Path(ses).write_text(_ses("X", -6000).replace(
            wire, wire + "(wire (path F.Cu 150  2850 -6000  3150 -6000))"))
        return 0

    route.route(path, router=stub)
    b = pcbnew.LoadBoard(str(path))
    lengths = sorted(round(pcbnew.ToMM(t.GetLength()), 2) for t in b.GetTracks())
    assert lengths == [18.0]


def test_the_session_moves_no_part(tmp_path):
    """Freerouting's session places every part on the DSN's 0.1 um grid, and pcbnew's import
    moved J1 from 37.574999 to 37.575 mm: a part moved by the route (P22), and a second route
    of the routed board then laid its rivers 1 nm aside (hw-repro failed). The import keeps
    every part where it was."""
    from hw.tests.test_finish import make_board
    b = make_board(tmp_path, {"X": [(3, 6), (21, 6)], "GND": [(22, 11)]})
    p1 = next(fp for fp in b.GetFootprints() if fp.GetReference() == "P1")
    p1.Move(pcbnew.VECTOR2I(-1, 0))                     # 1 nm off the DSN grid, pad and all
    path = tmp_path / "m.kicad_pcb"
    pcbnew.SaveBoard(str(path), b)
    before = {fp.GetReference(): (fp.GetPosition().x, fp.GetPosition().y)
              for fp in pcbnew.LoadBoard(str(path)).GetFootprints()}

    def stub(dsn, ses, passes, timeout_s, log, extra=()):
        Path(ses).write_text(_ses("X", -6000).replace(
            "(placement (resolution um 1))",
            '(placement (resolution um 10) (component "x" (place P1 0 0 front 0)))'))
        return 0

    route.route(path, router=stub)
    after = {fp.GetReference(): (fp.GetPosition().x, fp.GetPosition().y)
             for fp in pcbnew.LoadBoard(str(path)).GetFootprints()}
    assert after == before


def test_repair_relays_a_thinned_rail_at_its_class_width(tmp_path):
    from hw.tests.test_finish import make_board
    b = make_board(tmp_path, {"+5V": [(3, 6), (21, 6)], "GND": [(22, 11)]})
    _track(b, "+5V", pcbnew.F_Cu, (3, 6), (21, 6), 0.2)       # Freerouting's squeezed rail
    assert set(route.thin_power(b)) == {"+5V"}
    assert route.repair_clock_clearance(b) == []
    ts = [t for t in b.GetTracks()]
    assert ts and all(t.GetWidth() == pcbnew.FromMM(0.5) for t in ts if t.GetClass() != "PCB_VIA")


def test_a_rule_area_that_restricts_nothing_does_not_block_the_finisher(tmp_path):
    """J2_ESCAPE (build_board) only names an area for carrier.kicad_dru; the A* grid must
    not read it as a keep-out (it did, once, and no clock could be laid)."""
    from hw.tests.test_finish import make_board, v
    b = make_board(tmp_path, {"N": [(3, 6), (21, 6)], "GND": [(22, 11)]})
    z = pcbnew.ZONE(b)
    ls = pcbnew.LSET()
    ls.AddLayer(pcbnew.F_Cu)
    ls.AddLayer(pcbnew.B_Cu)
    z.SetLayerSet(ls)
    z.SetIsRuleArea(True)
    z.SetDoNotAllowTracks(False)
    z.Outline().NewOutline()
    for x, y in ((1, 1), (23, 1), (23, 11), (1, 11)):
        z.Outline().Append(pcbnew.FromMM(x), pcbnew.FromMM(y))
    b.Add(z)
    assert route._join_pieces(b, b.FindNet("N").GetNetCode(), 0.15, [])


def test_the_two_layer_ground_machinery_is_retired():
    """4 layers (2026-10-31): the planes carry GND, so the outer pours' stitching, bridging
    and the narrow GND class are gone, and so is route.py's --stitch flag and the justfile's."""
    for name in ("stitch_gnd", "bridge_gnd", "narrow_gnd", "_gnd_via", "STITCH_PITCH_MM"):
        assert not hasattr(route, name), name
    root = Path(__file__).resolve().parents[2]
    assert "--stitch" not in (root / "hw/justfile").read_text()
    with pytest.raises(SystemExit):
        route.main(["x.kicad_pcb", "--stitch"])


# --- Freerouting 2.5.0: the command line, the isolated config, the determinism ---------------

FAKE_ROUTER = """#!/bin/sh
printf '%s\\n' "$@" > "$FAKE_OUT/argv"
env > "$FAKE_OUT/env"
exit 0
"""


def _fake_router(tmp_path, monkeypatch):
    """A `freerouting` on PATH that records its arguments and environment."""
    bindir, out = tmp_path / "bin", tmp_path / "out"
    bindir.mkdir()
    out.mkdir()
    exe = bindir / "freerouting"
    exe.write_text(FAKE_ROUTER)
    exe.chmod(0o755)
    monkeypatch.setenv("PATH", f"{bindir}:" + os.environ["PATH"])
    monkeypatch.setenv("FAKE_OUT", str(out))
    return out


def test_freerouting_gets_the_2_5_flags(tmp_path, monkeypatch):
    out = _fake_router(tmp_path, monkeypatch)
    route.run_freerouting(tmp_path / "a.dsn", tmp_path / "a.ses", 7, 60, tmp_path / "log",
                          threads=3)
    argv = (out / "argv").read_text().split()
    assert argv[:4] == ["-de", str(tmp_path / "a.dsn"), "-do", str(tmp_path / "a.ses")]
    assert "--router.autorouter.max_passes=7" in argv
    assert "--router.optimizer.max_threads=3" in argv
    assert "--gui.enabled=false" in argv
    for old in ("-mp", "-mt", "-hr"):      # 2.4.1's (-mt alone does not cap the threads)
        assert old not in argv


def test_freerouting_gets_an_isolated_config(tmp_path, monkeypatch):
    """Java's user.home is the passwd entry, not $HOME, so HOME alone leaves the real
    ~/.config/freerouting open to writes: the XDG directories are pointed away too."""
    out = _fake_router(tmp_path, monkeypatch)
    route.run_freerouting(tmp_path / "a.dsn", tmp_path / "a.ses", 1, 60, tmp_path / "log")
    env = dict(l.split("=", 1) for l in (out / "env").read_text().splitlines() if "=" in l)
    real = os.path.realpath(os.path.expanduser("~"))     # (the build sandbox's is a temp dir)
    home = os.path.realpath(env["HOME"])
    assert home != real and not home.startswith(os.path.join(real, ".config"))
    for var in ("XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME"):
        assert os.path.realpath(env[var]).startswith(home + os.sep), var
    assert "-Duser.home=" + env["HOME"] in env["JAVA_TOOL_OPTIONS"]
    assert not Path(env["HOME"]).exists()      # a temporary directory, removed after


def _config_state():
    d = Path.home() / ".config" / "freerouting"
    return {str(p): (p.stat().st_mtime_ns, p.read_bytes()) for p in sorted(d.rglob("*"))
            if p.is_file()}


def _route_tiny(tmp_path, name):
    ses, log = tmp_path / f"{name}.ses", tmp_path / f"{name}.log"
    rc = route.run_freerouting(FIX / "tiny.dsn", ses, 3, 120, log)
    assert rc == 0, log.read_text()
    return ses, log


def test_freerouting_is_2_5_0_and_leaves_the_real_config_alone(tmp_path):
    before = _config_state()
    ses, log = _route_tiny(tmp_path, "one")
    assert "2.5.0" in log.read_text()
    assert ses.read_text().startswith("(session")
    assert _config_state() == before


def test_freerouting_output_is_identical_on_every_run(tmp_path):
    a, _ = _route_tiny(tmp_path, "one")
    b, _ = _route_tiny(tmp_path, "two")
    assert b"(wire" in a.read_bytes()       # a real route, not two empty sessions
    assert a.read_bytes() == b.read_bytes()
