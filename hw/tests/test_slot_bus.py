# SPDX-License-Identifier: GPL-3.0-or-later
"""The bus section as built: every net and part the hw-intent check and the board
depend on. Runs SKiDL once; the netlist is shared by the tests."""
import re
import subprocess
import sys
from pathlib import Path

import pytest

from hw.tools import check_intent as ci
from hw.tools import netlist as nlmod

ROOT = Path(__file__).resolve().parents[2]


@pytest.fixture(scope="module")
def built(tmp_path_factory):
    out = tmp_path_factory.mktemp("bus") / "slot_bus.net"
    from hw.src import slot_bus
    slot_bus.build_netlist(out)
    return out


@pytest.fixture(scope="module")
def nl(built):
    return nlmod.load(built)


def pins_on(nl, ref):
    return {pin: net for net, nodes in nl.nets.items() for r, pin in nodes if r == ref}


def by_value(nl, value):
    return sorted((r for r, p in nl.parts.items() if p.value == value),
                  key=lambda r: int(r[1:]))


def test_intent_is_clean_and_selftest_passes(nl):
    assert ci.problems(nl) == []
    assert ci.selftest(nl) == []


def test_cli_selftest_and_check(built):
    for args, ok in ([["--selftest"], "HW INTENT SELFTEST OK"], [[], "HW INTENT OK"]):
        r = subprocess.run([sys.executable, str(ROOT / "hw/tools/check_intent.py"),
                            *args, str(built)], capture_output=True, text=True, cwd=ROOT)
        assert r.returncode == 0, r.stdout + r.stderr
        assert ok in r.stdout.splitlines()


def test_j1_is_board_copper(nl):
    j1 = nl.parts["J1"]
    assert j1.footprint == "gsnix:AppleII_Slot_Edge"
    assert j1.fields["Fab"] == "pcb"
    assert "LCSC" not in j1.fields
    pins = pins_on(nl, "J1")
    assert len(pins) == 50 and set(pins) == {str(n) for n in range(1, 51)}


def test_every_other_part_has_lcsc_and_mpn(nl):
    for ref, p in nl.parts.items():
        if ref == "J1":
            continue
        assert re.fullmatch(r"C\d+", p.fields.get("LCSC", "")), ref
        assert p.fields.get("MPN"), ref
        assert p.footprint, ref
    assert nl.parts["JP2"].fields["Assembly"] == "hand"


def test_slot_pins_land_on_the_right_nets(nl):
    j = pins_on(nl, "J1")
    assert (j["1"], j["2"], j["17"], j["18"], j["19"], j["20"]) == (
        "SLOT_IOSEL_N", "SLOT_A0", "SLOT_A15", "SLOT_RW", "SLOT_SYNC_N", "SLOT_IOSTRB_N")
    assert (j["21"], j["22"], j["29"], j["30"], j["31"], j["32"]) == (
        "SLOT_RDY", "SLOT_DMA_N", "SLOT_NMI_N", "SLOT_IRQ_N", "SLOT_RESET_N",
        "SLOT_INH_N")
    assert (j["35"], j["36"], j["37"], j["38"], j["39"], j["40"], j["41"]) == (
        "SLOT_M2B0", "SLOT_7M", "SLOT_Q3", "SLOT_PH1", "SLOT_M2SEL_N", "SLOT_PH0",
        "SLOT_DEVSEL_N")
    assert (j["42"], j["49"]) == ("SLOT_D7", "SLOT_D0")
    assert (j["25"], j["26"], j["33"], j["34"], j["50"]) == (
        "+5V_SLOT", "GND", "-12V_SLOT", "-5V_SLOT", "+12V_SLOT")


def test_8t245_polarity(nl):
    """SN74LVC8T245 SCES584D table 7-1: OE=L DIR=L is B->A (slot -> FPGA); DIR and OE
    are referenced to VCCA. A side (pins 3-10) FPGA, B side (21..14) slot."""
    (u,) = by_value(nl, "SN74LVC8T245")
    p = pins_on(nl, u)
    assert p["1"] == "+3V3" and p["23"] == p["24"] == "+5V"
    assert p["2"] == "slot_d_dir" and p["22"] == "slot_d_oe_n"
    for i in range(8):
        assert p[str(3 + i)] == f"slot_d{i}" and p[str(21 - i)] == f"SLOT_D{i}"


def test_164245_polarity(nl):
    """SN74ALVC164245 SCAS416Q table 3: same function table, per section; control
    pins powered by VCCA. xA<k> (pins 47..37, 36..26) pair with xB<k> (2..12, 13..23)."""
    (u,) = by_value(nl, "74ALVC164245")
    p = pins_on(nl, u)
    assert p["31"] == p["42"] == "+3V3" and p["7"] == p["18"] == "+5V"
    assert p["1"] == p["24"] == "slot_a_dir" and p["48"] == p["25"] == "slot_a_oe_n"
    a_pins = [47, 46, 44, 43, 41, 40, 38, 37, 36, 35, 33, 32, 30, 29, 27, 26]
    b_pins = [2, 3, 5, 6, 8, 9, 11, 12, 13, 14, 16, 17, 19, 20, 22, 23]
    for i, (pa, pb) in enumerate(zip(a_pins, b_pins)):
        assert p[str(pa)] == f"slot_a{i}" and p[str(pb)] == f"SLOT_A{i}"


def test_1t45_polarity(nl):
    """SN74LVC1T45 SCES515N table 7-1: DIR=L is B->A. A = FPGA (VCCA, pin 1), B = slot."""
    (u,) = by_value(nl, "SN74LVC1T45")
    p = pins_on(nl, u)
    assert p == {"1": "+3V3", "2": "GND", "3": "slot_rw", "4": "SLOT_RW",
                 "5": "slot_a_dir", "6": "+5V"}


def test_541s(nl):
    u4, u5 = by_value(nl, "74LVC541")
    for u, pairs in ((u4, [(f"SLOT_D{i}", f"slot_dl{i}") for i in range(8)]),
                     (u5, [("SLOT_DEVSEL_N", "slot_devsel_n"),
                           ("SLOT_IOSEL_N", "slot_iosel_n"),
                           ("SLOT_IOSTRB_N", "slot_iostrb_n"),
                           ("SLOT_M2SEL_N", "slot_m2sel_n"), ("SLOT_M2B0", "slot_m2b0"),
                           ("SLOT_RESET_N", "slot_reset_n"),
                           ("SLOT_SYNC_N", "slot_sync_n")])):
        p = pins_on(nl, u)
        assert p["20"] == "+3V3" and p["10"] == "GND"
        assert p["1"] == "GND" and p["19"] == "GND"  # always enabled
        for i, (src, dst) in enumerate(pairs):
            assert p[str(2 + i)] == src and p[str(18 - i)] == dst
    assert pins_on(nl, u5)["9"] == "GND" and "11" not in pins_on(nl, u5)


def test_open_drain_outputs(nl):
    got = {}
    for u in by_value(nl, "74LVC1G07"):
        p = pins_on(nl, u)
        assert p["5"] == "+3V3" and p["3"] == "GND"
        got[p["2"]] = p["4"]
    assert got == {"slot_irq_n_o": "SLOT_IRQ_N", "slot_nmi_n_o": "SLOT_NMI_N",
                   "slot_inh_n_o": "SLOT_INH_N", "slot_dma_n_o": "SLOT_DMA_N",
                   "slot_rdy_o": "SLOT_RDY", "slot_reset_n_o": "SLOT_RESET_N"}


def test_clock_buffers_have_series_resistors(nl):
    got = {}
    for u in by_value(nl, "74LVC2G17"):
        p = pins_on(nl, u)
        assert p["5"] == "+3V3" and p["2"] == "GND"
        for a, y in (("1", "6"), ("3", "4")):
            (r,) = [r for r in nlmod.neighbours(nl, p[y]) if r.startswith("R")]
            got[p[a]] = nlmod.through(nl, r, p[y])
            assert nl.parts[r].value == "0"
    assert got == {"SLOT_PH0": "slot_ph0", "SLOT_PH1": "slot_ph1",
                   "SLOT_Q3": "slot_q3", "SLOT_7M": "slot_7m"}


def test_fuse_and_bypass(nl):
    assert pins_on(nl, "F1") == {"1": "+5V_SLOT", "2": "+5V"}
    assert nl.parts["F1"].value == "500mA"
    caps = {net: sorted(nl.parts[c].value for c in nlmod.neighbours(nl, net)
                        if c.startswith("C"))
            for net in ("+12V_SLOT", "-5V_SLOT", "-12V_SLOT", "+5V_SLOT")}
    for net in ("+12V_SLOT", "-5V_SLOT", "-12V_SLOT"):
        assert caps[net] == ["100nF"]
    assert "10uF" in {nl.parts[c].value for c in nlmod.neighbours(nl, "+5V")
                      if c.startswith("C")}
    assert "10uF" in {nl.parts[c].value for c in nlmod.neighbours(nl, "+3V3")
                      if c.startswith("C")}


def test_every_ic_supply_pin_has_a_bypass_cap(nl):
    n_supply = {"SN74LVC8T245": 2, "74ALVC164245": 4, "SN74LVC1T45": 2, "74LVC541": 1,
                "74LVC2G17": 1, "74LVC1G07": 1}
    need = sum(n * len(by_value(nl, v)) for v, n in n_supply.items())
    have = sum(1 for r, p in nl.parts.items() if r.startswith("C") and p.value == "100nF"
               and {nlmod.net_of(nl, r, "1"), nlmod.net_of(nl, r, "2")} & {"+3V3", "+5V"})
    assert have >= need


def test_all_passives_0603(nl):
    for r, p in nl.parts.items():
        if r[0] in "RC":
            assert "0603" in p.footprint, r


def test_netlist_is_reproducible(tmp_path_factory, built):
    from hw.src import slot_bus
    again = slot_bus.build_netlist(tmp_path_factory.mktemp("bus2") / "slot_bus.net")
    a = [ln for ln in built.read_text().splitlines() if "SKiDL Line" not in ln and "(source" not in ln and "(date" not in ln]
    b = [ln for ln in again.read_text().splitlines() if "SKiDL Line" not in ln and "(source" not in ln and "(date" not in ln]
    assert a == b


def test_no_stray_files_at_repo_root(built):
    stray = [p.name for p in ROOT.iterdir()
             if p.suffix in (".erc", ".log") or p.name.endswith("_sklib.py")]
    assert stray == []
