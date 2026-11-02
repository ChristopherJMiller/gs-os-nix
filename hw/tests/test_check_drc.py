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
