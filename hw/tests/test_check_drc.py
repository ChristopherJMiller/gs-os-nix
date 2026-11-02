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
    """Two N1 pads joined by one F.Cu track, a GND pad, and `extra(b)` planted; saved. The
    hand-made parts' reference texts are hidden: they all sit at the board's corner."""
    from hw.tests.test_route import _track
    b = make_board(tmp_path, {"N1": [(4, 6), (20, 6)], "GND": [(22, 11)]})
    for fp in b.GetFootprints():
        for f in fp.GetFields():
            f.SetVisible(False)
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


# --- the silkscreen: KiCad warns, hw-drc fails ------------------------------------------------

def _silk_text(b, s, x, y):
    t = pcbnew.PCB_TEXT(b)
    t.SetLayer(pcbnew.F_SilkS)
    t.SetText(s)
    t.SetTextSize(v(1.0, 1.0))
    t.SetTextThickness(pcbnew.FromMM(0.15))
    t.SetPosition(v(x, y))
    b.Add(t)


def test_silk_on_a_pad_fails(tmp_path):
    """Silkscreen on a pad is clipped by the solder mask opening (KiCad's silk_over_copper): a
    reference designator printed over a pad is half lost, and the carrier had 52 of them."""
    path = _drc_board(tmp_path, lambda b: _silk_text(b, "X", 20, 6))
    found = check_drc.problems_drc(path)
    assert any("DRC silk_over_copper" in p for p in found), found


def test_silk_on_silk_fails(tmp_path):
    """Two silkscreen items on top of each other (silk_overlap): neither reads."""
    def plant(b):
        _silk_text(b, "X", 12, 9)
        _silk_text(b, "X", 12.2, 9)
    found = check_drc.problems_drc(_drc_board(tmp_path, plant))
    assert any("DRC silk_overlap" in p for p in found), found


def test_silk_over_the_board_edge_fails(tmp_path):
    path = _drc_board(tmp_path, lambda b: _silk_text(b, "XX", 23.9, 9))
    found = check_drc.problems_drc(path)
    assert any("DRC silk_edge_clearance" in p for p in found), found


def test_silk_clear_of_pads_silk_and_edge_passes(tmp_path):
    path = _drc_board(tmp_path, lambda b: _silk_text(b, "X", 12, 9))
    assert check_drc.problems_drc(path) == []


# --- footprints against their libraries: the DRC run is given a fp-lib-table ------------------

def _library_part(b, fpid="Resistor_SMD:R_0603_1608Metric"):
    from hw.tools import build_board
    fp = build_board.load_footprint(fpid)
    fp.SetReference("R1")
    fp.SetPosition(v(12, 9.5))
    for f in fp.GetFields():
        f.SetVisible(False)
    b.Add(fp)
    return fp


def test_a_footprint_as_in_its_library_passes(tmp_path):
    """The DRC run finds the board's libraries (a fp-lib-table beside the temporary copy), so a
    library footprint is checked against its library and passes: no lib_footprint_issues."""
    assert check_drc.problems_drc(_drc_board(tmp_path, _library_part)) == []


def test_a_footprint_unlike_its_library_fails(tmp_path):
    def plant(b):
        p = list(_library_part(b).Pads())[0]
        p.SetSize(F, p.GetSize(F) + v(0.05, 0.05))
    found = check_drc.problems_drc(_drc_board(tmp_path, plant))
    assert any("DRC lib_footprint_mismatch" in p for p in found), found


def test_a_footprint_from_no_library_fails(tmp_path):
    def plant(b):
        _library_part(b).SetFPID(pcbnew.LIB_ID("NoSuchLibrary", "R_0603_1608Metric"))
    found = check_drc.problems_drc(_drc_board(tmp_path, plant))
    assert any("DRC lib_footprint_issues" in p for p in found), found


def test_the_gate_fails_the_silk_and_library_warnings():
    assert {"silk_over_copper", "silk_overlap", "silk_edge_clearance", "lib_footprint_issues",
            "lib_footprint_mismatch"} <= set(check_drc.GATED_WARNINGS)


def test_the_selftest_plants_silk_and_library_faults():
    expects = {expect for _, (_, drc, expect) in check_drc.MUTATIONS.items() if drc}
    assert {"DRC silk_over_copper", "DRC silk_overlap", "DRC silk_edge_clearance",
            "DRC lib_footprint_mismatch", "DRC lib_footprint_issues"} <= expects
