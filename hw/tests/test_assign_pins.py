# SPDX-License-Identifier: GPL-3.0-or-later
"""assign_pins: ratsnest crossings and the FPGA pin re-assignment they score."""
import subprocess
import sys
from pathlib import Path

import pcbnew
import pytest

from hw.tools import assign_pins as ap
from hw.tools import module as modmod
from hw.tools import netlist as nlmod
from hw.tools import pins as pinsmod

ROOT = Path(__file__).resolve().parents[2]
CARRIER = ROOT / "hw" / "carrier"
BOARD = CARRIER / "carrier.kicad_pcb"
PINS = CARRIER / "pins.toml"
MODULE = CARRIER / "icesugar_pro_v1_3.toml"


def mm(v):
    return pcbnew.FromMM(v)


def synthetic(pads: dict[str, tuple[str, float, float]], nets: dict[str, list[str]]):
    """A board of one-pad parts and its netlist. pads: ref -> (net, x, y); nets: net ->
    refs (each part's pad is '1')."""
    board = pcbnew.BOARD()
    for ref, (net, x, y) in pads.items():
        fp = pcbnew.FOOTPRINT(board)
        fp.SetReference(ref)
        fp.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y)))
        pad = pcbnew.PAD(fp)
        pad.SetNumber("1")
        pad.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y)))
        fp.Add(pad)
        board.Add(fp)
    nl = nlmod.Netlist()
    for net, refs in nets.items():
        nl.nets[net] = {(r, "1") for r in refs}
    return board, nl


def test_two_crossing_nets_count_one():
    board, nl = synthetic(
        {"A": ("n1", 0, 0), "B": ("n1", 10, 10), "C": ("n2", 0, 10), "D": ("n2", 10, 0)},
        {"n1": ["A", "B"], "n2": ["C", "D"]})
    assert ap.crossings(board, nl) == 1


def test_parallel_nets_count_zero():
    board, nl = synthetic(
        {"A": ("n1", 0, 0), "B": ("n1", 10, 10), "C": ("n2", 0, 1), "D": ("n2", 10, 11)},
        {"n1": ["A", "B"], "n2": ["C", "D"]})
    assert ap.crossings(board, nl) == 0


def test_a_net_uses_its_minimum_spanning_tree():
    """Three pads in a row: the tree is the two short hops, so a wire across the first hop
    crosses once (a full clique would also count the long P0-P2 edge)."""
    board, nl = synthetic(
        {"P0": ("n1", 0, 0), "P1": ("n1", 4, 0), "P2": ("n1", 8, 0),
         "Q0": ("n2", 2, -3), "Q1": ("n2", 2, 3)},
        {"n1": ["P0", "P1", "P2"], "n2": ["Q0", "Q1"]})
    assert ap.crossings(board, nl) == 1


def test_segments_touching_at_an_end_do_not_cross():
    board, nl = synthetic(
        {"A": ("n1", 0, 0), "B": ("n1", 10, 0), "C": ("n2", 5, 0), "D": ("n2", 5, 5)},
        {"n1": ["A", "B"], "n2": ["C", "D"]})
    assert ap.crossings(board, nl) == 0


def test_ground_and_rails_are_not_counted():
    board, nl = synthetic(
        {"A": ("n1", 0, 0), "B": ("n1", 10, 10), "C": ("GND", 0, 10), "D": ("GND", 10, 0)},
        {"n1": ["A", "B"], "GND": ["C", "D"]})
    assert ap.crossings(board, nl) == 0


# --- the real carrier --------------------------------------------------------------

@pytest.fixture(scope="module")
def carrier(tmp_path_factory):
    out = tmp_path_factory.mktemp("carrier")
    subprocess.run([sys.executable, "-m", "hw.carrier.top", "--out", str(out),
                    "--no-schematic"], cwd=ROOT, check=True, capture_output=True)
    return pcbnew.LoadBoard(str(BOARD)), nlmod.load(out / "carrier.net")


@pytest.fixture(scope="module")
def proposal(carrier):
    board, nl = carrier
    pm, mod = pinsmod.load(PINS), modmod.load(MODULE)
    return ap.propose(board, nl, mod, pm, iters=4000, starts=2), pm, mod


def test_propose_never_reuses_a_ball_and_stays_on_3v3_pio(proposal):
    new, old, mod = proposal
    seen = {}
    tre = ap.check_pinmap.Trellis(new.part)
    for p in new.ports.values():
        for net, _, ball in pinsmod.bits(p):
            assert ball not in seen, f"{ball} on both {seen[ball]} and {net}"
            seen[ball] = net
            if not p.on_module:
                assert ball in mod.balls.values(), f"{net}: {ball} is not on the SODIMM"
            assert ball in tre.bank, f"{net}: {ball} is not a PIO"
            assert mod.bank_vccio[tre.bank[ball]] == 3.3, f"{net}: {ball} is not 3.3 V"


def test_propose_keeps_ports_directions_and_module_balls(proposal):
    new, old, _ = proposal
    assert list(new.ports) == list(old.ports)
    for name, p in old.ports.items():
        q = new.ports[name]
        assert (q.dir, len(q.balls), q.on_module, q.io_standard) == (
            p.dir, len(p.balls), p.on_module, p.io_standard)
        if p.on_module:
            assert q.balls == p.balls


def test_propose_does_not_make_crossings_worse(carrier, proposal):
    board, nl = carrier
    new, old, mod = proposal
    assert ap.crossings_for(board, nl, mod, new) <= ap.crossings_for(board, nl, mod, old)


def test_propose_keeps_bus_bits_in_order(proposal):
    """Each multi-bit bus runs monotonically along the socket (ascending or descending)."""
    new, _, mod = proposal
    for name in ("slot_a", "slot_d", "slot_dl"):
        pins = [modmod.pin_for_ball(mod, b) for b in new.ports[name].balls]
        assert pins == sorted(pins) or pins == sorted(pins, reverse=True), (name, pins)


def test_cli_report_and_write(tmp_path):
    dst = tmp_path / "pins.toml"
    dst.write_text(PINS.read_text())
    r = subprocess.run([sys.executable, str(ROOT / "hw/tools/assign_pins.py"), "--report",
                        "--board", str(BOARD), "--pins", str(dst), "--module", str(MODULE),
                        "--iters", "2000", "--starts", "1"],
                       capture_output=True, text=True, cwd=ROOT)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "current crossings:" in r.stdout and "proposed crossings:" in r.stdout
    assert dst.read_text() == PINS.read_text()          # --report writes nothing
    r = subprocess.run([sys.executable, str(ROOT / "hw/tools/assign_pins.py"), "--write",
                        "--board", str(BOARD), "--pins", str(dst), "--module", str(MODULE),
                        "--iters", "2000", "--starts", "1"],
                       capture_output=True, text=True, cwd=ROOT)
    assert r.returncode == 0, r.stdout + r.stderr
    written = pinsmod.load(dst)
    assert sorted(written.ports) == sorted(pinsmod.load(PINS).ports)
    assert dst.read_text().startswith("# SPDX-License-Identifier: CERN-OHL-S-2.0")
