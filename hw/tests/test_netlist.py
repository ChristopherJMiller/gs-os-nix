# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path

import pytest

from hw.tools import netlist

FIX = Path(__file__).parent / "fixtures" / "tiny.net"


def test_parts_and_fields():
    nl = netlist.load(FIX)
    r1 = nl.parts["R1"]
    assert r1.value == "1k"
    assert r1.footprint == "Resistor_SMD:R_0603_1608Metric"
    assert r1.fields["LCSC"] == "C21190"
    assert r1.fields["Note"] == 'has (parens) and "quotes"'
    assert "LCSC" not in nl.parts["R2"].fields
    assert nl.parts["R2"].value == "2k"


def test_nets():
    nl = netlist.load(FIX)
    assert nl.nets["N1"] == {("R1", "1"), ("R2", "1")}
    assert nl.nets["GND"] == {("R1", "2"), ("R2", "2")}


def test_net_of_and_neighbours():
    nl = netlist.load(FIX)
    assert netlist.net_of(nl, "R1", "1") == "N1"
    assert netlist.net_of(nl, "R1", "9") is None
    assert netlist.neighbours(nl, "N1") == {"R1", "R2"}


def test_through():
    nl = netlist.load(FIX)
    assert netlist.through(nl, "R1", "N1") == "GND"
    with pytest.raises(ValueError):
        netlist.through(nl, "R1", "NOPE")


def test_through_rejects_non_two_pin_and_reads_bare_atoms(tmp_path):
    p = tmp_path / "x.net"
    p.write_text('(export (components (comp (ref U1) (value x) (footprint f)))'
                 ' (nets (net (code 1) (name A) (node (ref U1) (pin 1)))'
                 ' (net (code 2) (name "B (x)") (node (ref U1) (pin 2)))'
                 ' (net (code 3) (name C) (node (ref U1) (pin 3)))))')
    nl = netlist.load(p)
    assert nl.parts["U1"].fields == {}
    assert "B (x)" in nl.nets
    with pytest.raises(ValueError):
        netlist.through(nl, "U1", "A")
