# SPDX-License-Identifier: GPL-3.0-or-later
"""hw-drc's own rules on small boards: P28, no via over an SMD pad."""
import pcbnew

from hw.tests.test_finish import F, make_board, v
from hw.tools import check_drc


def _via(b, net, x, y):
    x_ = pcbnew.PCB_VIA(b)
    x_.SetPosition(v(x, y))
    x_.SetViaType(pcbnew.VIATYPE_THROUGH)
    x_.SetLayerPair(F, pcbnew.B_Cu)
    x_.SetWidth(pcbnew.FromMM(0.6))
    x_.SetDrill(pcbnew.FromMM(0.3))
    x_.SetNet(b.FindNet(net))
    b.Add(x_)


def test_a_via_over_an_smd_pad_fails_whatever_its_net(tmp_path):
    b = make_board(tmp_path, {"GND": [(6, 6)], "N1": [(12, 6)]})
    _via(b, "GND", 6, 6)                   # its own pad (the C14 case)
    _via(b, "N1", 12.5, 6)                 # overlapping its own pad's edge
    found = check_drc.problems_via_in_pad(b)
    assert len(found) == 2 and all("via in pad" in p for p in found), found
    assert any("P1.1" in p for p in found) and any("P2.1" in p for p in found)


def test_a_via_beside_a_pad_passes(tmp_path):
    b = make_board(tmp_path, {"GND": [(6, 6)], "N1": [(12, 6)]})
    _via(b, "GND", 6, 6.75)                # 0.35 mm from the pad edge: copper does not touch
    assert check_drc.problems_via_in_pad(b) == []


def test_the_selftest_plants_a_via_in_a_pad():
    name, (mutate, drc, expect) = next((k, m) for k, m in check_drc.MUTATIONS.items()
                                       if "via" in k and "pad" in k)
    assert not drc and expect == "via in pad"
    b = make_board(None, {"GND": [(6, 6)], "N1": [(12, 6)]})
    mutate(b)
    assert any(expect in p for p in check_drc.problems_via_in_pad(b))


# --- dangling copper: KiCad warns, hw-drc fails ----------------------------------------------

def _drc_board(tmp_path, extra):
    """Two N1 pads joined by one F.Cu track, a GND pad, and `extra(b)` planted; saved."""
    from hw.tests.test_route import _track
    b = make_board(tmp_path, {"N1": [(4, 6), (20, 6)], "GND": [(22, 11)]})
    b.GetDesignSettings().m_TrackMinWidth = pcbnew.FromMM(0.15)      # the carrier's minimum
    _track(b, "N1", F, (4, 6), (20, 6))
    extra(b)
    path = tmp_path / "d.kicad_pcb"
    pcbnew.SaveBoard(str(path), b)
    return path


def test_a_via_joined_on_one_layer_only_fails(tmp_path):
    """Freerouting left eight vias on the carrier with copper on one side only (a pass-through,
    or the end of a dead branch); KiCad calls them via_dangling, a warning. hw-drc fails them."""
    path = _drc_board(tmp_path, lambda b: _via(b, "N1", 12, 6))
    found = check_drc.problems_drc(path)
    assert any("DRC via_dangling" in p for p in found), found


def test_a_track_with_a_free_end_fails(tmp_path):
    from hw.tests.test_route import _track
    path = _drc_board(tmp_path, lambda b: _track(b, "N1", F, (12, 6), (12, 8)))
    found = check_drc.problems_drc(path)
    assert any("DRC track_dangling" in p for p in found), found


def test_a_clean_board_has_no_drc_problem(tmp_path):
    assert check_drc.problems_drc(_drc_board(tmp_path, lambda b: None)) == []


def test_the_selftest_plants_a_dangling_via_and_a_dangling_track():
    expects = {expect for _, (_, drc, expect) in check_drc.MUTATIONS.items() if drc}
    assert {"DRC via_dangling", "DRC track_dangling"} <= expects
