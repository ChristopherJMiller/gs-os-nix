# SPDX-License-Identifier: GPL-3.0-or-later
"""hw-congestion: the capacity arithmetic, the negotiated router on a synthetic board, each
planted fault failing for its own reason, and the calibration boards (plan 1's routed board must
pass, since it routed)."""
import shutil
import subprocess
import sys
from pathlib import Path

import pcbnew
import pytest

from hw.tools import check_congestion as cc

ROOT = Path(__file__).resolve().parents[2]


def git_show(rev: str, path: str, out: Path) -> bool:
    r = subprocess.run(["git", "show", f"{rev}:{path}"], cwd=ROOT, capture_output=True)
    if r.returncode != 0:
        return False
    out.write_bytes(r.stdout)
    return True


def test_run_capacity_is_the_open_edge_rate_for_a_free_edge():
    # a 0.5 mm edge with both ends open carries 0.5 / 0.30 tracks: capacity 500 um
    assert cc.run_capacity_um(10, open_lo=True, open_hi=True) == 500


def test_run_capacity_gives_a_bounded_run_the_end_bonus():
    # a gap of one free sample between two obstacles still takes one track (300 um)
    assert cc.run_capacity_um(1, open_lo=False, open_hi=False) >= 300
    # one end open, one bounded: half the bonus
    two = cc.run_capacity_um(6, False, False)
    one = cc.run_capacity_um(6, True, False)
    zero = cc.run_capacity_um(6, True, True)
    assert zero < one < two


def test_a_fully_blocked_edge_has_no_capacity():
    assert cc.run_capacity_um(0, False, False) == 0


def test_synthetic_board_passes():
    res = cc.analyze(cc.synthetic())
    assert cc.problems(res) == []
    assert res.overflow == 0


@pytest.mark.parametrize("name", list(cc.PLANT))
def test_each_planted_fault_fails_for_its_own_reason(name):
    board = cc.synthetic()
    cc.PLANT[name](board)
    res = cc.analyze(board)
    assert cc.problems(res), "the planted fault passed"
    for other, test in cc.EXPECT.items():
        assert bool(test(res)) == (other == name), (name, other, cc.problems(res))


def test_the_result_is_deterministic():
    a = cc.analyze(cc.synthetic())
    b = cc.analyze(cc.synthetic())
    assert (a.overflow, a.wirelength) == (b.overflow, b.wirelength)


def test_heatmap_is_written(tmp_path):
    res = cc.analyze(cc.synthetic())
    png = tmp_path / "h.png"
    cc.write_heatmap(res, png)
    assert png.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n"


def test_selftest_passes(capsys):
    assert cc.main(["--selftest"]) == 0
    assert "HW CONGESTION SELFTEST OK" in capsys.readouterr().out


def test_a_wide_power_net_alone_fits_a_gap_that_takes_two_signals():
    """The capacity is an average over cells: any net alone fits an edge that has capacity."""
    b = cc._new_board()
    cc._rect_outline(b, 0, 0, 40, 20)
    cc._keepout(b, 19, 0, 21, 9.75)
    cc._keepout(b, 19, 10.25, 21, 20)               # a 0.5 mm gap
    cc._pads(b, "L", ["+5V"], 5.0, 10.0)
    cc._pads(b, "R", ["+5V"], 35.0, 10.0)
    res = cc.analyze(b)
    assert cc.problems(res) == [], cc.problems(res)


def test_the_committed_board_is_modelled_with_its_plane_fanout():
    board = pcbnew.LoadBoard(str(ROOT / "hw" / "carrier" / "carrier.kicad_pcb"))
    assert cc.prepare(board) == []
    vias = [t for t in board.GetTracks() if t.GetClass() == "PCB_VIA"]
    assert len(vias) >= 50                       # a via for every GND and +3V3 SMD pad
    m = cc.build_model(board)
    assert m.planes == {"GND", "+3V3"} and not m.pours
    assert not {n.name for n in m.nets} & m.planes


@pytest.mark.parametrize("gap,passes", [(30.0, True), (8.0, False)])
def test_a_wall_across_the_committed_board_fails_when_its_gap_is_too_narrow(gap, passes):
    """The board's ~100 signals cross y = 50 on the way from the module to the translators; a
    keep-out wall there with a 30 mm gap leaves them room, one with 8 mm does not."""
    board = pcbnew.LoadBoard(str(ROOT / "hw" / "carrier" / "carrier.kicad_pcb"))
    cc._keepout(board, 0, 50, 40 - gap / 2, 51)
    cc._keepout(board, 40 + gap / 2, 50, 80, 51)
    res = cc.analyze(board)
    assert (cc.problems(res) == []) == passes, cc.problems(res)
    if not passes:
        assert res.overflow > 5
        assert all(48 < y < 53 for y in cc._y(res)[:5])     # the worst edges are at the wall


def test_a_run_on_the_committed_board_raises_no_kicad_assert(capfd):
    board = pcbnew.LoadBoard(str(ROOT / "hw" / "carrier" / "carrier.kicad_pcb"))
    capfd.readouterr()
    cc.analyze(board)
    assert "GetWidth" not in capfd.readouterr().err


def test_plan1_routed_board_passes(tmp_path):
    """Plan 1's board (7430efe) routed on two layers, so the gate must not fail it."""
    pcb = tmp_path / "plan1.kicad_pcb"
    if not (git_show("7430efe", "hw/carrier/carrier.kicad_pcb", pcb)
            and git_show("7430efe", "hw/carrier/carrier.kicad_pro", pcb.with_suffix(".kicad_pro"))):
        pytest.skip("commit 7430efe is not in this checkout")
    res = cc.analyze(pcbnew.LoadBoard(str(pcb)))
    assert cc.problems(res) == [], cc.problems(res)
