# SPDX-License-Identifier: GPL-3.0-or-later
"""hw-load: the capacitance table covers the carrier's parts, the arithmetic on a synthetic
board, and each planted fault tripping on its own."""
import copy
import subprocess
import sys
from pathlib import Path

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


def test_every_table_entry_is_cited(caps):
    for value, e in caps.items():
        assert e["url"].startswith("https://"), value
        assert e["table"], value
        assert e["basis"] in ("typ", "max"), value
        assert e["ci"] > 0, value


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
    assert rows["SLOT_X"]["total"] == pytest.approx(10.0)
    # A header counts 1 pF.
    assert rows["SLOT_JP"]["pins"] == pytest.approx(1.0)
    assert cl.problems(board, nl, caps) == []


def test_in_and_out_of_a_daisy_chain_are_one_load(caps):
    board, nl = cl.synthetic(caps)
    rows = {r["net"]: r for r in cl.table(board, nl, caps)}
    assert rows["SLOT_CH_IN"]["total"] == rows["SLOT_CH_OUT"]["total"] == pytest.approx(2.0)


def test_limit_and_scale_are_parameters(caps):
    board, nl = cl.synthetic(caps)
    assert any("SLOT_D0" in p for p in cl.problems(board, nl, caps, limit_pf=12.0))
    assert any("SLOT_D0" in p for p in cl.problems(board, nl, caps, pf_per_mm=0.5))


def test_a_missing_table_entry_is_a_problem(caps):
    board, nl = cl.synthetic(caps)
    short = {k: v for k, v in caps.items() if k != "74LVC541"}
    assert any("74LVC541" in p and "no capacitance entry" in p
               for p in cl.problems(board, nl, short))


def test_long_track_trips():
    caps = cl.load_caps(TABLE)
    board, nl = cl.synthetic(caps)
    cl.lengthen_track(board, "SLOT_D0", 60.0)
    found = cl.problems(board, nl, caps)
    assert any(cl.EXPECT["track lengthened by 60 mm"] in p for p in found), found


def test_third_buffer_trips():
    caps = cl.load_caps(TABLE)
    board, nl = cl.synthetic(caps)
    nl = copy.deepcopy(nl)
    cl.add_third_buffer(nl, "SLOT_D0")
    found = cl.problems(board, nl, caps)
    assert any(cl.EXPECT["third buffer input on SLOT_D0"] in p for p in found), found


def test_raised_ci_trips():
    caps = cl.load_caps(TABLE)
    board, nl = cl.synthetic(caps)
    bad = copy.deepcopy(caps)
    bad["74LVC541"]["ci"] = 20.0
    found = cl.problems(board, nl, bad)
    assert any(cl.EXPECT["74LVC541 Ci raised to 20 pF"] in p for p in found), found


def test_the_three_messages_differ():
    assert len(set(cl.EXPECT.values())) == 3


def test_cli_selftest(capsys):
    assert cl.main(["--selftest"]) == 0
    assert "HW LOAD SELFTEST OK" in capsys.readouterr().out
