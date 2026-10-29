# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path

import pytest

from hw.tools import pins

FIX = Path(__file__).parent / "fixtures"


def test_bus_bits_expand():
    pm = pins.load(FIX / "pins_ok.toml")
    assert pm.part == "LFE5U-25F-6BG256C"
    assert pins.bits(pm.ports["slot_a"])[3] == ("slot_a3", "slot_a[3]", "B4")
    assert pins.bits(pm.ports["uart_tx"]) == [("uart_tx", "uart_tx", "F3")]
    assert pm.ports["clk25"].on_module and not pm.ports["slot_a"].on_module


def test_lpf_lines():
    lines = pins.to_lpf(pins.load(FIX / "pins_ok.toml")).splitlines()
    assert 'LOCATE COMP "slot_a[0]" SITE "B3";' in lines
    assert 'IOBUF PORT "slot_a[0]" IO_TYPE=LVCMOS33;' in lines
    assert 'LOCATE COMP "uart_tx" SITE "F3";' in lines


def test_duplicate_ball_rejected(tmp_path):
    p = tmp_path / "p.toml"
    p.write_text('part = "X"\nio_standard = "LVCMOS33"\n'
                 '[ports.a]\ndir = "in"\nballs = ["B3"]\n'
                 '[ports.b]\ndir = "out"\nballs = ["B3"]\n')
    with pytest.raises(ValueError, match="B3"):
        pins.load(p)


def test_verilog_ports():
    assert pins.verilog_ports(FIX / "ports.v", "top") == {
        "clk25": ("input", 1),
        "slot_a": ("input", 16),
        "slot_d": ("inout", 8),
        "uart_tx": ("output", 1),
    }


def test_verilog_ports_several_names_per_line(tmp_path):
    v = tmp_path / "m.v"
    v.write_text("module m (\n  input  wire  slot_ph0, slot_q3, slot_7m,\n"
                 "  output reg [3:0] q\n);\nendmodule\n")
    assert pins.verilog_ports(v, "m") == {
        "slot_ph0": ("input", 1), "slot_q3": ("input", 1), "slot_7m": ("input", 1),
        "q": ("output", 4)}


def test_verilog_ports_real_card():
    root = Path(__file__).resolve().parents[2]
    ports = pins.verilog_ports(root / "rtl" / "a2nix_card.v", "a2nix_card")
    assert ports["c0n_addr"] == ("input", 4)
    assert ports["mem_addr"] == ("output", 32)
    assert ports["irq"] == ("output", 1)
