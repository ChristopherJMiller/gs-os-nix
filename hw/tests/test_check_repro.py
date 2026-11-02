# SPDX-License-Identifier: GPL-3.0-or-later
"""hw-repro (check_repro.py): routing the committed board again gives its copper, track for
track. The route itself takes minutes, so here a stub stands in for it; the real run is the
justfile's `repro` recipe."""
from pathlib import Path

import pcbnew

from hw.tests.test_finish import F, B, make_board
from hw.tools import check_repro

ROOT = Path(__file__).resolve().parents[2]


def _routed(tmp_path):
    b = make_board(tmp_path, {"N1": [(4, 6), (18, 6)], "+5V": [(4, 9), (18, 9)]},
                   tracks=[("N1", F, (4, 6), (10, 6), 0.15), ("N1", B, (10, 6), (18, 6), 0.15),
                           ("+5V", F, (4, 9), (18, 9), 0.5)],
                   vias=[("N1", (10, 6))])
    path = tmp_path / "r.kicad_pcb"
    pcbnew.SaveBoard(str(path), b)
    return path


def _first(board, net, kind):
    return next(t for t in board.GetTracks()
                if t.GetNetname() == net and (t.GetClass() == "PCB_VIA") == (kind == "via"))


def test_the_same_copper_passes(tmp_path):
    path = _routed(tmp_path)
    seen = []

    def same(copy, passes):                 # a route that lays what was there: nothing changes
        seen.append((copy, passes))

    assert check_repro.check(path, 7, run=same) == []
    copy, passes = seen[0]
    assert copy != path and passes == 7     # a copy is routed, never the board itself


def test_a_track_end_moved_by_a_micron_is_reported_with_its_net(tmp_path):
    path = _routed(tmp_path)

    def nudge(copy, passes):
        b = pcbnew.LoadBoard(str(copy))
        t = _first(b, "+5V", "track")
        t.SetEnd(t.GetEnd() + pcbnew.VECTOR2I(1000, 0))
        pcbnew.SaveBoard(str(copy), b)

    problems = check_repro.check(path, 1, run=nudge)
    assert problems and all("+5V" in p for p in problems), problems


def test_a_missing_via_and_a_wider_track_are_reported(tmp_path):
    path = _routed(tmp_path)

    def change(copy, passes):
        b = pcbnew.LoadBoard(str(copy))
        b.RemoveNative(_first(b, "N1", "via"))
        _first(b, "+5V", "track").SetWidth(pcbnew.FromMM(0.6))
        pcbnew.SaveBoard(str(copy), b)

    problems = check_repro.check(path, 1, run=change)
    assert any("via N1" in p and "only in BOARD" in p for p in problems), problems
    assert any("+5V" in p and "only in the re-route" in p for p in problems), problems


def test_a_route_that_leaves_nets_is_still_compared(tmp_path):
    path = _routed(tmp_path)

    def gives_up(copy, passes):
        raise SystemExit("route: left unrouted:\n  N1: no path")

    assert check_repro.check(path, 1, run=gives_up) == []


def test_the_selftest_catches_each_plant(tmp_path):
    assert check_repro.selftest(_routed(tmp_path)) == []


def test_the_selftest_plants_a_lock_flag_flipped(tmp_path):
    """bus_route's and the first pass's copper is locked, the router's is not: a re-route that
    locked a track differently is not the committed board."""
    assert any("lock" in name for name in check_repro.PLANT)
    b = pcbnew.LoadBoard(str(_routed(tmp_path)))
    ours = check_repro.signature(b)
    net = check_repro.PLANT[next(n for n in check_repro.PLANT if "lock" in n)](b)
    assert any(f" {net} " in p for p in check_repro.compare(ours, check_repro.signature(b)))


def test_compare_counts_a_track_laid_twice():
    """Two identical tracks are two lines: a re-route that lays one twice differs."""
    assert check_repro.compare(["track A"], ["track A", "track A"]) == \
        ["only in the re-route: track A"]
    assert check_repro.compare(["track A", "track A"], ["track A"]) == \
        ["only in BOARD: track A"]


def test_the_signature_tells_two_arcs_apart_by_their_middle(tmp_path):
    from hw.tests.test_finish import v

    def arc_board(mid_y):
        b = make_board(tmp_path, {"N1": [(4, 6), (20, 6)]})
        a = pcbnew.PCB_ARC(b)
        a.SetStart(v(4, 6))
        a.SetMid(v(12, mid_y))
        a.SetEnd(v(20, 6))
        a.SetLayer(F)
        a.SetWidth(pcbnew.FromMM(0.15))
        a.SetNet(b.FindNet("N1"))
        b.Add(a)
        return check_repro.signature(b)

    assert arc_board(4.0) != arc_board(3.0)


def test_the_reroute_uses_the_justfile_route_arguments():
    """`repro` must route as `route` does: the same route.py flags as the justfile's recipe."""
    text = (ROOT / "hw" / "justfile").read_text()

    def recipe(name):
        body = text[text.index(f"\n{name} "):]
        return body[:body.index("\n\n")]

    call = recipe("route")
    call = call[call.index("hw/tools/route.py"):]
    words = call[:call.index("&&")].replace("\\", " ").split()
    assert words[:4] == ["hw/tools/route.py", "hw/carrier/carrier.kicad_pcb", "--passes",
                         "{{PASSES}}"]
    assert words[4:] == check_repro.ROUTE_ARGS
    assert "check_repro.py" in recipe("repro") and "{{PASSES}}" in recipe("repro")
