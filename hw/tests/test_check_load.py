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
    assert len(set(cl.EXPECT.values())) == len(cl.EXPECT) >= 9


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


# --- the via term (4 layers, plan 1b amendment) -----------------------------------------

def test_stackup_numbers_come_from_the_table_and_are_cited():
    import tomllib
    t = tomllib.loads(TABLE.read_text())
    for sec in ("stackup", "via"):
        assert t[sec]["source"], sec
    # the table's via and plane numbers are the board builder's
    from hw.tools import build_board as bb
    assert t["via"]["pad_mm"] == bb.VIA_MM
    assert t["via"]["antipad_mm"] == pytest.approx(bb.VIA_MM + 2 * bb.INNER_CLEARANCE_MM)
    assert t["stackup"]["er"] == 4.4 and t["stackup"]["prepreg_mm"] == 0.2104


def test_microstrip_pf_per_mm_follows_ipc_2141():
    # C0 = 0.67 (er + 1.41) / ln(5.98 h / (0.8 w + t)) pF/in, h = 0.2104, w = 0.15, t = 0.035
    got = cl.microstrip_pf_per_mm(er=4.4, h_mm=0.2104, w_mm=0.15, t_mm=0.035)
    assert got == pytest.approx(0.0732, abs=0.0005)
    assert cl.MICROSTRIP_PF_PER_MM == pytest.approx(got, abs=1e-9)
    # 0.10 stays while the computed figure is below it, and the figure is recorded
    assert cl.PF_PER_MM == 0.10 and cl.MICROSTRIP_PF_PER_MM <= 0.10
    assert "IPC-2141" in cl.__doc__ and "pF/mm" in cl.__doc__


def test_via_capacitance_follows_the_standard_formula():
    # C = 1.41 er T D1 / (D2 - D1) pF, inches: T = 1.6 mm, D1 = 0.6 mm, D2 = 1.0 mm
    got = cl.via_pf(er=4.4, t_mm=1.6, d1_mm=0.6, d2_mm=1.0)
    t, d1, d2 = 1.6 / 25.4, 0.6 / 25.4, 1.0 / 25.4
    assert got == pytest.approx(1.41 * 4.4 * t * d1 / (d2 - d1))
    assert got == pytest.approx(0.586, abs=0.002)
    assert cl.VIA_PF == pytest.approx(got, abs=1e-9)
    assert "Johnson" in cl.__doc__


def test_vias_on_a_net_and_its_zero_ohm_twin_are_counted(caps):
    board, nl = cl.synthetic(caps)
    for net, n in (("SLOT_D0", 2), ("ph0_in", 1)):
        for k in range(n):
            cl._via(board, net, 5.0 + k, 20.0)
    rows = {r["net"]: r for r in cl.table(board, nl, caps)}
    assert rows["SLOT_D0"]["vias"] == 2 and rows["SLOT_D0"]["via_pf"] == pytest.approx(2 * cl.VIA_PF)
    assert rows["SLOT_D0"]["total"] == pytest.approx(3.0 + 9.5 + 2 * cl.VIA_PF)
    assert rows["SLOT_PH0"]["vias"] == 1          # ph0_in is joined to SLOT_PH0 by a 0 ohm part


def test_the_via_plant_fails_only_through_its_vias(caps):
    board, nl = cl.synthetic(caps)
    assert cl.problems(board, nl, caps) == []     # without the vias, D1 is within the limit
    found = _planted("5 vias on SLOT_D1", caps)
    assert len(found) == 1 and cl.EXPECT["5 vias on SLOT_D1"] in found[0], found
    assert "IC pins" in found[0]
