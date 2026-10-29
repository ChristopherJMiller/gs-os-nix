# SPDX-License-Identifier: GPL-3.0-or-later
"""check_intent on a small synthetic netlist (so the carrier's J2 rules, which have no
real J2 until Task 6, are tested now), and on the real bus section."""
import copy

import pytest

from hw.tools import check_intent as ci
from hw.tools.module import ModuleMap
from hw.tools.netlist import Netlist, PartInfo

MODULE = ModuleMap(rails={"5V": [2, 4, 6], "GND": [1, 3, 5]}, rail_role={}, balls={},
                   bank_vccio={})


def _add(nl: Netlist, ref: str, value: str, pins: dict[str, str]) -> None:
    nl.parts[ref] = PartInfo(value, "", {})
    for pin, net in pins.items():
        nl.nets.setdefault(net, set()).add((ref, pin))


def synthetic(with_module: bool = False) -> Netlist:
    """The smallest netlist every bus rule passes on: one of each part, wired as
    slot_bus wires it."""
    nl = Netlist()
    j1 = {"25": "+5V_SLOT", "26": "GND", "33": "-12V_SLOT", "34": "-5V_SLOT",
          "50": "+12V_SLOT", "28": "SLOT_INT_IN", "23": "SLOT_INT_OUT",
          "27": "SLOT_DMA_IN", "24": "SLOT_DMA_OUT"}
    j1.update({str(42 + i): f"SLOT_D{7 - i}" for i in range(8)})
    _add(nl, "J1", "Slot", j1)
    _add(nl, "F1", "500mA", {"1": "+5V_SLOT", "2": "+5V"})
    _add(nl, "C1", "100n", {"1": "+5V_SLOT", "2": "GND"})
    for n, net in enumerate(["-12V_SLOT", "-5V_SLOT", "+12V_SLOT"], start=2):
        _add(nl, f"C{n}", "100n", {"1": net, "2": "GND"})
    _add(nl, "R1", "0", {"1": "SLOT_INT_IN", "2": "SLOT_INT_OUT"})
    _add(nl, "R2", "0", {"1": "SLOT_DMA_IN", "2": "SLOT_DMA_OUT"})
    pulls = [("slot_d_oe_n", "+3V3"), ("slot_a_oe_n", "+3V3"), ("slot_d_dir", "GND"),
             ("slot_a_dir", "GND"), ("slot_reset_n_o", "GND")]
    pulls += [(n, "+3V3") for n in ci.RELEASED_NETS]
    for i, (net, rail) in enumerate(pulls, start=10):
        _add(nl, f"R{i}", "10k", {"1": net, "2": rail})
    _add(nl, "R30", "1k", {"1": "JP2_3V3", "2": "+3V3"})
    _add(nl, "JP2", "Jumper", {"1": "slot_reset_n_o", "2": "JP2_3V3"})
    d = {"1": "+3V3", "23": "+5V", "24": "+5V"}
    d.update({str(3 + i): f"slot_d{i}" for i in range(8)})
    d.update({str(21 - i): f"SLOT_D{i}" for i in range(8)})
    _add(nl, "U1", "SN74LVC8T245", d)
    _add(nl, "U2", "74ALVC164245", {"31": "+3V3", "42": "+3V3", "7": "+5V", "18": "+5V"})
    _add(nl, "U3", "SN74LVC1T45", {"1": "+3V3", "6": "+5V"})
    _add(nl, "U4", "74LVC541", {"20": "+3V3", **{str(2 + i): f"SLOT_D{i}" for i in range(8)}})
    _add(nl, "U5", "74LVC2G17", {"5": "+3V3"})
    _add(nl, "U6", "74LVC1G07", {"5": "+3V3"})
    if with_module:
        mod = {str(p): "+5V_MOD" for p in MODULE.rails["5V"]}
        mod.update({str(p): "GND" for p in MODULE.rails["GND"]})
        _add(nl, "J2", "SODIMM", mod)
        _add(nl, "D1", "Schottky", {"2": "+5V", "1": "+5V_MOD"})
    return nl


def test_synthetic_is_clean():
    assert ci.problems(synthetic()) == []
    assert ci.problems(synthetic(True), MODULE) == []


@pytest.mark.parametrize("name", sorted(ci.MUTATIONS))
def test_every_bus_mutation_trips_its_rule(name):
    nl = copy.deepcopy(synthetic())
    ci.MUTATIONS[name](nl)
    assert ci.problems(nl), f"{name} was not caught"


@pytest.mark.parametrize("name", sorted(ci.MODULE_MUTATIONS))
def test_every_module_mutation_trips_its_rule(name):
    nl = synthetic(True)
    ci.MODULE_MUTATIONS[name](nl, MODULE)
    assert ci.problems(nl, MODULE), f"{name} was not caught"


def test_module_rules_are_off_without_a_module():
    nl = synthetic(True)
    ci.MODULE_MUTATIONS["module_diode_shorted"](nl, MODULE)
    assert ci.problems(nl) == []


def test_module_mutations_name_their_fault():
    for name, needle in [("module_5v_pin_on_wrong_net", "J2.2 (module 5V)"),
                         ("module_gnd_pin_floating", "J2.5 (module GND)"),
                         ("module_diode_shorted", "bridges"),
                         ("module_second_bridge", "bridges"),
                         ("module_diode_reversed", "wrong way round")]:
        nl = synthetic(True)
        ci.MODULE_MUTATIONS[name](nl, MODULE)
        assert any(needle in p for p in ci.problems(nl, MODULE)), name


def test_selftest_reports_an_unnoticed_mutation(monkeypatch):
    monkeypatch.setitem(ci.MUTATIONS, "does_nothing", lambda nl: None)
    assert any("does_nothing produced no problem" in f for f in ci.selftest(synthetic()))


def test_selftest_clean_on_synthetic():
    assert ci.selftest(synthetic(True), MODULE) == []


def test_zero_ohm_is_not_a_pullup():
    nl = synthetic()
    r = ci._resistor(nl, "slot_d_oe_n", "+3V3")
    nl.parts[r] = PartInfo("0", "", {})
    assert any("slot_d_oe_n has no pull-up" in p for p in ci.problems(nl))


def test_jp2_straight_to_3v3_is_accepted():
    nl = synthetic()
    ci._drop(nl, "R30")
    ci._move(nl, "JP2", "2", "+3V3")
    assert ci.problems(nl) == []
