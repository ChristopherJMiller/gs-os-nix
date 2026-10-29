# SPDX-License-Identifier: GPL-3.0-or-later
"""The carrier top: its parts, power, module wiring and the generated outputs."""
import re
import subprocess
from pathlib import Path

import pytest

from hw.tools import module as modmod
from hw.tools import netlist as nlmod
from hw.tools import pins as pinsmod

ROOT = Path(__file__).resolve().parents[2]
PINS = ROOT / "hw" / "carrier" / "pins.toml"
MODULE = ROOT / "hw" / "carrier" / "icesugar_pro_v1_3.toml"
TOP_V = ROOT / "rtl" / "board" / "carrier_top.v"


@pytest.fixture(scope="module")
def carrier(tmp_path_factory):
    """The carrier netlist, built in a scratch dir (SKiDL's side files stay there)."""
    out = tmp_path_factory.mktemp("carrier")
    subprocess.run(["python3", "-m", "hw.carrier.top", "--out", str(out), "--no-schematic"], cwd=ROOT,
                   check=True, capture_output=True)
    return out


@pytest.fixture(scope="module")
def nl(carrier):
    return nlmod.load(carrier / "carrier.net")


def test_outputs_exist(carrier):
    for f in ("carrier.net", "carrier.lpf", "erc.txt"):  # the schematic: hw-carrier-build
        assert (carrier / f).is_file(), f
    assert "0 errors" in (carrier / "erc.txt").read_text()


def test_lpf_is_generated_from_pins_toml(carrier):
    assert (carrier / "carrier.lpf").read_text() == pinsmod.to_lpf(pinsmod.load(PINS))


def test_every_part_has_an_lcsc_number_but_the_edge_connector(nl):
    exempt = {r for r, p in nl.parts.items() if p.fields.get("Fab") == "pcb"}
    assert exempt == {"J1"}
    for ref, p in nl.parts.items():
        if ref in exempt:
            continue
        assert re.fullmatch(r"C\d+", p.fields.get("LCSC", "")), f"{ref}: {p.fields}"
        assert p.fields.get("MPN"), ref


def test_headers_are_hand_assembled(nl):
    for ref in ("J4", "JP1", "JP2"):
        assert nl.parts[ref].fields.get("Assembly") == "hand", ref


def test_no_jtag_header(nl):
    assert "J3" not in nl.parts  # R11: the module routes JTAG nowhere but its own USB


def test_socket_is_the_sodimm_footprint(nl):
    assert nl.parts["J2"].footprint == "gsnix:SODIMM200_DDR2"


def test_every_bit_reaches_the_socket(nl):
    pm, mod = pinsmod.load(PINS), modmod.load(MODULE)
    for p in pm.ports.values():
        if p.on_module:
            continue
        for net, _, ball in pinsmod.bits(p):
            pin = modmod.pin_for_ball(mod, ball)
            assert nlmod.net_of(nl, "J2", str(pin)) == net


def test_only_the_assigned_and_power_pins_carry_a_net(nl):
    """Every other J2 pin is no-connect: it sits on SKiDL's no-connect net (or none)."""
    mod = modmod.load(MODULE)
    pm = pinsmod.load(PINS)
    wired = {modmod.pin_for_ball(mod, b) for p in pm.ports.values() if not p.on_module
             for b in p.balls}
    rails = {pin for pins in mod.rails.values() for pin in pins}
    for pin in range(1, 201):
        net = nlmod.net_of(nl, "J2", str(pin))
        if pin in wired or pin in rails:
            assert net and "NC" not in net.upper().split("_")[0], (pin, net)
        else:
            assert net is None or net.startswith("NC"), (pin, net)


def test_module_power(nl):
    mod = modmod.load(MODULE)
    for pin in mod.rails["5V"]:
        assert nlmod.net_of(nl, "J2", str(pin)) == "+5V_MOD"
    for pin in mod.rails["GND"]:
        assert nlmod.net_of(nl, "J2", str(pin)) == "GND"
    assert nlmod.net_of(nl, "D1", "1") == "+5V_MOD"  # Device:D: pin 1 is the cathode
    assert nlmod.net_of(nl, "D1", "2") == "+5V"      # pin 2 the anode
    assert nlmod.net_of(nl, "VR1", "5") == "+3V3"     # AP2112K-3.3 pin 5 = VOUT
    assert nlmod.net_of(nl, "VR1", "1") == "+5V_MOD"  # pin 1 = VIN
    assert nlmod.net_of(nl, "VR1", "3") == "+5V_MOD"  # pin 3 = EN, always on
    assert nlmod.net_of(nl, "VR1", "2") == "GND"


def test_module_bulk_capacitor(nl):
    caps = [r for r in nlmod.neighbours(nl, "+5V_MOD")
            if r.startswith("C") and nl.parts[r].value in ("22uF", "100nF")]
    assert {nl.parts[r].value for r in caps} == {"22uF", "100nF"}


def test_uart_lines_have_series_resistors(nl):
    for fpga, header, pin in (("uart_tx", "UART_TX", "1"), ("uart_rx", "UART_RX", "2")):
        assert nlmod.net_of(nl, "J4", pin) == header
        res = [r for r in nlmod.neighbours(nl, header) if r.startswith("R")]
        assert len(res) == 1 and nl.parts[res[0]].value == "100"
        assert nlmod.through(nl, res[0], header) == fpga
    assert nlmod.net_of(nl, "J4", "3") == "GND"


def test_safe_jumper_grounds_a_pulled_up_net(nl):
    assert nlmod.net_of(nl, "JP1", "1") == "safe_n"
    assert nlmod.net_of(nl, "JP1", "2") == "GND"
    pulls = [r for r in nlmod.neighbours(nl, "safe_n")
             if r.startswith("R") and nlmod.through(nl, r, "safe_n") == "+3V3"]
    assert pulls


def test_carrier_top_lints_clean():
    r = subprocess.run(["verilator", "--lint-only", "-Wall", str(TOP_V)],
                       capture_output=True, text=True)
    assert r.returncode == 0 and "%Warning" not in r.stderr, r.stderr


def test_balls_within_one_bank_per_bus():
    """Each bus is one bank (the grouping the comment in pins.toml promises)."""
    import json
    import os
    mod = modmod.load(MODULE)
    db = json.loads((Path(os.environ["TRELLIS_DB"]) / "ECP5" / "LFE5U-25F"
                     / "iodb.json").read_text())
    bank = {(e["row"], e["col"], e["pio"]): e["bank"] for e in db["pio_metadata"]}
    cab = db["packages"]["CABGA256"]
    pm = pinsmod.load(PINS)
    for name in ("slot_a", "slot_d", "slot_dl"):
        banks = {bank[(cab[b]["row"], cab[b]["col"], cab[b]["pio"])]
                 for b in pm.ports[name].balls}
        assert len(banks) == 1, (name, banks)
        pins = [modmod.pin_for_ball(mod, b) for b in pm.ports[name].balls]
        assert pins == sorted(pins), name  # bit i on the i-th contact of the run
        assert pins[-1] - pins[0] < 40, name
