# SPDX-License-Identifier: GPL-3.0-or-later
import json
import os
from pathlib import Path

import pytest

from hw.tools import module

TOML = Path(__file__).resolve().parents[1] / "carrier" / "icesugar_pro_v1_3.toml"

# Baseboard header balls, P2-P6, transcribed from doc/iCESugar-pro-pinmap.png
# (wuxx/icesugar-pro 087e48d9). P1 carries only power, GND and NC.
P2 = "F2 D1 C1 B1 B3 B4 B5 B6 B7 B8  E1 C2 B2 A2 A3 A4 A5 A6 A7 A8"
P3 = "P7 R6 T4 T3 T2 R1 P1 N1 M1 K1  T6 R5 R4 R3 R2 P2 N3 M2 L2 G2"
P4 = "R7 R8 D5 C4 D4 C3 E4 E3 F4 F3 G4 G3 K4 J4 H3 J3 K3 J2"
P5 = "D6 D7 D8 D9 D10 D11 D12 D13 E12 E14  C5 C6 C7 C8 C9 C10 C11 C12 C13 E13"
P6 = "F14 G14 H14 J14 K14 L14 M12 N13 N12 P11  F13 G13 H13 J13 K13 L13 M13 P14 P13 P12"
HEADER_BALLS = set(" ".join([P2, P3, P4, P5, P6]).split())

GPDI = "E2 D3 G1 F1 J1 H2 L1 K2".split()
SDRAM = ("R15 H15 B13 B12 J16 J15 R12 K16 R13 T13 K15 A13 R14 T14 "
         "F16 E15 F15 D14 E16 C15 D16 B15 R16 P16 P15 N16 N14 M16 M15 L15 "
         "A15 B16 G16 A14 L16 G15 B14 C16 T15").split()
FLASH = "N8 T8 T7".split()
SERIAL = "B9 A9".split()


@pytest.fixture(scope="module")
def m():
    return module.load(TOML)


@pytest.fixture(scope="module")
def iodb():
    p = Path(os.environ["TRELLIS_DB"]) / "ECP5" / "LFE5U-25F" / "iodb.json"
    return json.loads(p.read_text())


def test_literals_sane():
    assert len(SDRAM) == 39 and len(set(SDRAM)) == 39
    assert len(HEADER_BALLS) == 20 + 20 + 18 + 20 + 20


def test_each_pin_once(m):
    seen = []
    for pins in m.rails.values():
        seen += pins
    seen += list(m.balls)
    assert all(1 <= p <= 200 for p in seen)
    assert len(seen) == len(set(seen)), "a SODIMM pin is used twice"
    assert len(set(m.balls.values())) == len(m.balls), "a ball is on two pins"


def test_balls_are_pios(m, iodb):
    cabga = iodb["packages"]["CABGA256"]
    for pin, ball in m.balls.items():
        assert ball in cabga, f"pin {pin}: {ball} is not a PIO of CABGA256"


def test_banks_are_3v3(m, iodb):
    meta = {(e["row"], e["col"], e["pio"]): e["bank"] for e in iodb["pio_metadata"]}
    cabga = iodb["packages"]["CABGA256"]
    for pin, ball in m.balls.items():
        c = cabga[ball]
        bank = meta[(c["row"], c["col"], c["pio"])]
        assert m.bank_vccio.get(bank) == 3.3, f"pin {pin} ball {ball} bank {bank}"


def test_baseboard_headers_reach_sodimm(m):
    missing = HEADER_BALLS - set(m.balls.values())
    assert not missing, sorted(missing)


def test_baseboard_functions_reach_sodimm(m):
    assert set(GPDI) <= set(m.balls.values())


def test_module_functions_stay_on_module(m):
    on = set(m.balls.values())
    assert not (set(SDRAM) & on)
    assert not (set(FLASH) & on)
    assert not (set(SERIAL) & on)


def test_power_roles(m):
    assert set(m.rails) == {"5V", "GND"}
    assert m.rail_role["5V"] in ("input", "input+usb")
    assert set(m.rail_role) == set(m.rails)


def test_known_pins(m):
    # spot checks read straight off the schematic image
    for pin, ball in {41: "A8", 42: "P11", 87: "D3", 91: "E2", 155: "R8", 156: "K3"}.items():
        assert module.pin_for_ball(m, ball) == pin
    assert m.rails["5V"] == [2, 4, 6, 8, 10, 12]
    with pytest.raises(KeyError):
        module.pin_for_ball(m, "B9")


def test_notes_and_source(m):
    raw = TOML.read_text()
    assert "087e48d9e0b0a0168ce165a961cba306335c4cf2" in raw
    assert "767cd3f1ac434a0c54d279bd41c885f86024656cf8da36d733530fb07a232b4a" in raw
    assert raw.startswith("# SPDX-License-Identifier: CERN-OHL-S-2.0")
    assert "iCELink" in m.notes["jtag"] and "iCELink" in m.notes["serial"]
