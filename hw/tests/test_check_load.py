# SPDX-License-Identifier: GPL-3.0-or-later
"""hw-load: the capacitance table covers the carrier's parts, the arithmetic on a synthetic
board, and each planted fault tripping on its own."""
import copy
import subprocess
import sys
from pathlib import Path

import pcbnew
import pytest

from hw.tools import check_load as cl
from hw.tools import netlist as nlmod

ROOT = Path(__file__).resolve().parents[2]
TABLE = ROOT / "hw" / "carrier" / "pin_capacitance.toml"


@pytest.fixture(scope="module")
def caps():
    return cl.load_caps(TABLE)


@pytest.fixture(scope="module")
def carrier_nl(tmp_path_factory):
    out = tmp_path_factory.mktemp("carrier")
    subprocess.run([sys.executable, "-m", "hw.carrier.top", "--out", str(out), "--no-schematic"],
                   cwd=ROOT, check=True, capture_output=True)
    return nlmod.load(out / "carrier.net")


def test_table_covers_every_ic_value_on_a_j1_net(caps, carrier_nl):
    values = cl.ic_values_on_j1_nets(carrier_nl)
    assert {"SN74LVCR2245A", "74ALVC164245", "SN74LVC1T45", "74LVC541", "74LVC2G17",
            "74LVC1G07"} <= values
    assert values - set(caps) == set()


def test_every_ic_pin_on_a_j1_net_is_listed_in_the_table(caps, carrier_nl):
    """The real netlist trips none of the unknown-pin / unknown-part / empty-net guards
    (copper is not asserted here: the board is a separate matter)."""
    rows = cl.table(cl.empty_board(), carrier_nl, caps)
    for r in rows:
        assert not r["missing"] and not r["unlisted"] and not r["unknown"], r
        assert r["ic_pins"] or r["header_pins"] or r["joined"], r["net"]


def test_every_table_entry_is_cited(caps):
    for value, e in caps.items():
        assert e["url"].startswith("https://"), value
        assert e["table"], value
        assert e["basis"] in ("typ", "max"), value
        for pins, cap in (("in_pins", "ci"), ("io_pins", "cio"), ("od_pins", "co")):
            if e.get(pins):
                assert e[cap] > 0, (value, cap)
        assert e.get("in_pins") or e.get("io_pins"), value


def test_the_carrier_has_a_row_per_j1_signal_net(caps, carrier_nl):
    rows = cl.table(cl.empty_board(), carrier_nl, caps)
    names = {r["net"] for r in rows}
    assert {"SLOT_D0", "SLOT_D7", "SLOT_PH0", "SLOT_RESET_N", "SLOT_DMA_IN"} <= names
    assert not names & set(cl.POWER_NETS)


def test_arithmetic_on_a_synthetic_board(caps):
    board, nl = cl.synthetic(caps)
    rows = {r["net"]: r for r in cl.table(board, nl, caps)}
    # D0: 30 mm; U1.18 is a B-side I/O pin (Cio 5.5), U4.2 a 541 input (Ci 4).
    assert rows["SLOT_D0"]["copper"] == pytest.approx(3.0)
    assert rows["SLOT_D0"]["pins"] == pytest.approx(caps["SN74LVCR2245A"]["cio"]
                                                    + caps["74LVC541"]["ci"])
    assert rows["SLOT_D0"]["total"] == pytest.approx(3.0 + 9.5)
    # PH0 reaches U6 through a 0 ohm part: 20 + 10 mm of copper, one 2G17 input.
    assert rows["SLOT_PH0"]["copper"] == pytest.approx(3.0)
    assert rows["SLOT_PH0"]["pins"] == pytest.approx(caps["74LVC2G17"]["ci"])
    # A 1k resistor stops the walk: its far side (500 mm, a 541 input) is not counted.
    assert rows["SLOT_X"]["total"] == pytest.approx(10.0 + 4.0)
    # A header counts 1 pF.
    assert rows["SLOT_JP"]["pins"] == pytest.approx(1.0)
    # An open-drain output uses its Co.
    assert rows["SLOT_OD"]["pins"] == pytest.approx(caps["74LVC1G07"]["co"])
    assert cl.problems(board, nl, caps) == []


def test_in_and_out_of_a_daisy_chain_are_one_load(caps):
    board, nl = cl.synthetic(caps)
    rows = {r["net"]: r for r in cl.table(board, nl, caps)}
    assert rows["SLOT_CH_IN"]["total"] == rows["SLOT_CH_OUT"]["total"] == pytest.approx(2.0)


def test_limit_and_scale_are_parameters(caps):
    board, nl = cl.synthetic(caps)
    assert any("SLOT_D0" in p for p in cl.problems(board, nl, caps, limit_pf=12.0))
    assert any("SLOT_D0" in p for p in cl.problems(board, nl, caps, pf_per_mm=0.5))


def _planted(name, caps):
    """The problems that fault `name` produces on the synthetic board."""
    board, nl = cl.synthetic(caps)
    caps = copy.deepcopy(caps)
    cl.PLANT[name](board, nl, caps)
    return cl.problems(board, nl, caps)


@pytest.mark.parametrize("name", list(cl.EXPECT))
def test_each_planted_fault_trips_with_its_own_message(name, caps):
    found = _planted(name, caps)
    assert any(cl.EXPECT[name] in p for p in found), found
    for other, text in cl.EXPECT.items():
        if other != name:
            assert not any(text in p for p in found), (name, other, found)


def test_the_messages_differ():
    assert len(set(cl.EXPECT.values())) == len(cl.EXPECT) >= 8


def test_an_unrouted_board_fails_every_net(caps):
    _, nl = cl.synthetic(caps)
    found = cl.problems(cl.empty_board(), nl, caps)
    assert sum("no copper" in p for p in found) == len(cl.j1_nets(nl))


def test_slash_prefixed_names_match_on_both_sides(caps):
    board, nl = cl.synthetic(caps)
    board.FindNet("SLOT_D0").SetNetname("/SLOT_D0")
    assert cl.problems(board, nl, caps) == []
    board, nl = cl.synthetic(caps)
    nl.nets["/SLOT_D1"] = nl.nets.pop("SLOT_D1")
    assert cl.problems(board, nl, caps) == []


def test_cli_selftest(capsys):
    assert cl.main(["--selftest"]) == 0
    assert "HW LOAD SELFTEST OK" in capsys.readouterr().out
