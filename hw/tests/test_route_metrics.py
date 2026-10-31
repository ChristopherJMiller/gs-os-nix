# SPDX-License-Identifier: GPL-3.0-or-later
"""route_metrics: the In1 GND-plane fill under the bus row (antipad voids) and the via
counts of a 4-layer board."""
import pcbnew

from hw.tests.test_finish import F, B, make_board, v
from hw.tools import route_metrics


def _board(tmp_path, with_via: bool):
    b = make_board(tmp_path, {"GND": [(6, 6)], "N1": [(15, 6)], "+3V3": [(18, 6)]},
                   vias=[("N1", (15, 6))] if with_via else [])
    z = pcbnew.ZONE(b)
    z.SetLayer(pcbnew.In1_Cu)
    z.SetNet(b.FindNet("GND"))
    z.SetIsRuleArea(False)
    z.AppendCorner(v(1, 1), -1)
    for x, y in ((23, 1), (23, 11), (1, 11)):
        z.AppendCorner(v(x, y), -1)
    b.Add(z)
    ds = b.GetDesignSettings()
    ds.m_MinClearance = pcbnew.FromMM(0.5)             # a wide antipad round the N1 via
    z.SetLocalClearance(pcbnew.FromMM(0.5))
    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    return b


def test_in1_fill_loses_the_antipad_of_a_signal_via(tmp_path):
    rows = ("P1", "P2", "P3")                           # the box round the three pads
    clean = route_metrics.plane_fill(_board(tmp_path, False), pcbnew.In1_Cu, "GND", rows)
    holed = route_metrics.plane_fill(_board(tmp_path, True), pcbnew.In1_Cu, "GND", rows)
    assert 50.0 < clean < 100.0       # the box is thin: the other pads' own clearances show
    assert holed < clean - 1.0                          # the via's antipad is a void


def test_via_counts_split_plane_fanout_from_the_rest(tmp_path):
    b = make_board(tmp_path, {"GND": [(6, 6)], "N1": [(15, 6)]},
                   vias=[("GND", (7, 3)), ("N1", (15, 3)), ("N1", (16, 3))])
    assert route_metrics.via_counts(b, {"GND", "+3V3"}) == (3, 1)
