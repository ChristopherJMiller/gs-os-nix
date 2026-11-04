# SPDX-License-Identifier: GPL-3.0-or-later
"""plan_buses (Task 5f): the FPGA-side buses as order-preserving rivers onto J2."""
import itertools
import random
import subprocess
import sys
from pathlib import Path

import pcbnew
import pytest

from hw.tools import check_pinmap
from hw.tools import module as modmod
from hw.tools import pins as pinsmod
from hw.tools import plan_buses as pb

ROOT = Path(__file__).resolve().parents[2]
CARRIER = ROOT / "hw" / "carrier"
BOARD = CARRIER / "carrier.kicad_pcb"
PINS = CARRIER / "pins.toml"
MODULE = CARRIER / "icesugar_pro_v1_3.toml"
# pins.toml as Task 5c left it (assign_pins' ratsnest annealing), kept to show what changed
PINS_5C = Path(__file__).parent / "fixtures" / "pins-5c.toml"


@pytest.fixture(scope="module")
def board():
    return pcbnew.LoadBoard(str(BOARD))


@pytest.fixture(scope="module")
def mod():
    return modmod.load(MODULE)


# --- the dynamic programme --------------------------------------------------------------

def _brute(srcs, pads):
    best = None
    for combo in itertools.combinations(range(len(pads)), len(srcs)):
        cost = sum(abs(pads[j][0] - s) for j, s in zip(combo, srcs))
        if best is None or cost < best[0] - 1e-9:
            best = (cost, [pads[j][1] for j in combo])
    return best


def test_monotone_keeps_the_order_and_is_optimal():
    rng = random.Random(7)
    for _ in range(200):
        m = rng.randint(1, 8)
        n = rng.randint(1, m)
        pads = sorted((round(rng.uniform(0, 20), 2), k) for k in range(m))
        srcs = sorted(round(rng.uniform(0, 20), 2) for _ in range(n))
        got = pb.monotone(srcs, pads)
        xs = {c: x for x, c in pads}
        assert [xs[c] for c in got] == sorted(xs[c] for c in got)       # order kept
        assert len(set(got)) == n
        cost = sum(abs(xs[c] - s) for c, s in zip(got, srcs))
        assert cost == pytest.approx(_brute(srcs, pads)[0])


def test_monotone_breaks_ties_to_the_lower_pads():
    assert pb.monotone([5.0], [(4.0, 1), (6.0, 2)]) == [1]
    assert pb.monotone([1.0, 2.0], [(1.0, 1), (2.0, 2), (3.0, 3)]) == [1, 2]


def test_monotone_refuses_more_nets_than_pads():
    with pytest.raises(ValueError):
        pb.monotone([1.0, 2.0], [(1.0, 1)])


# --- the carrier --------------------------------------------------------------------------

def test_the_5c_assignment_is_not_a_river(board, mod):
    """RED for the old file (the reason for 5f): today's buses cross themselves under the
    topology bus_route lays (the data bus on both rows, inversions in the address bus)."""
    probs = pb.river_problems(board, mod, pinsmod.load(PINS_5C))
    assert any(p.startswith("data:") and "row" in p for p in probs), probs
    assert any("cross" in p for p in probs), probs


def test_the_committed_pins_are_rivers(board, mod):
    assert pb.river_problems(board, mod, pinsmod.load(PINS)) == []


def test_pins_toml_is_the_plan(board, mod):
    """pins.toml is generated (do not hand-edit): it is exactly plan_buses' output."""
    pm = pinsmod.load(PINS)
    assert PINS.read_text() == pb.render(pb.plan(board, mod, pm), mod)


def test_the_plan_is_planar_and_complete(board, mod):
    pm = pinsmod.load(PINS_5C)
    new = pb.plan(board, mod, pm)
    assert pb.river_problems(board, mod, new) == []
    nets = {n for r in pb.RIVERS for n in r.nets}
    wired = pb.wiring(mod, new)
    assert set(wired) == nets                    # every J2 bit is in exactly one river
    assert sum(len(r.nets) for r in pb.RIVERS) == len(nets)


def test_the_plan_keeps_hw_pinmaps_rules(board, mod):
    pm = pinsmod.load(PINS_5C)
    new = pb.plan(board, mod, pm)
    tre = check_pinmap.Trellis(new.part)
    seen = set()
    for p in new.ports.values():
        old = pm.ports[p.name]
        assert (p.dir, len(p.balls), p.on_module) == (old.dir, len(old.balls), old.on_module)
        if p.on_module:
            assert p.balls == old.balls
        for ball in p.balls:
            assert ball not in seen
            seen.add(ball)
            assert ball in tre.bank and mod.bank_vccio[tre.bank[ball]] == 3.3
            if not p.on_module:
                assert ball in mod.balls.values()


def test_the_plan_travels_less_than_the_5c_assignment(board, mod):
    old = pinsmod.load(PINS_5C)
    assert pb.travel(board, mod, pb.plan(board, mod, old)) < pb.travel(board, mod, old)


def test_the_plan_is_the_same_on_every_run(board, mod):
    pm = pinsmod.load(PINS_5C)
    assert pb.render(pb.plan(board, mod, pm), mod) == pb.render(pb.plan(board, mod, pm), mod)


def test_rows_follow_the_rivers_access(board, mod):
    pads = pb.sodimm_pads(board)
    wired = pb.wiring(mod, pinsmod.load(PINS))
    for r in pb.RIVERS:
        if r.row != pb.ANY:
            assert {pads[wired[n]][1] for n in r.nets} == {r.row}, r.name


def test_the_slot_side_is_already_monotone_per_finger_side(board):
    """5f's slot half: along each side of the fingers, the translator pins a bus reaches are
    in the fingers' order (Task 1 and Tasks 3+4 chose the channels), so each finger side's
    river is planar. A guard: it held before 5f."""
    j1 = board.FindFootprintByReference("J1")
    fingers = {p.GetNetname(): (pcbnew.ToMM(p.GetX()), "F" if p.IsOnLayer(pcbnew.F_Cu) else "B")
               for p in j1.Pads() if p.GetNetname().startswith("SLOT_")}
    for ref in ("U1", "U2", "U4", "U5"):
        fp = board.FindFootprintByReference(ref)
        pin_x = {p.GetNetname(): pcbnew.ToMM(p.GetX()) for p in fp.Pads()
                 if p.GetNetname() in fingers}
        for side in ("F", "B"):
            nets = sorted((n for n in pin_x if fingers[n][1] == side),
                          key=lambda n: fingers[n][0])
            xs = [pin_x[n] for n in nets]
            assert xs == sorted(xs), (ref, side, nets)


def test_cli_report_and_write(tmp_path):
    dst = tmp_path / "pins.toml"
    dst.write_text(PINS_5C.read_text())
    r = subprocess.run([sys.executable, str(ROOT / "hw/tools/plan_buses.py"), "--report",
                        "--board", str(BOARD), "--pins", str(dst), "--module", str(MODULE)],
                       capture_output=True, text=True, cwd=ROOT)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "current:" in r.stdout and "planned: 0 river problems" in r.stdout
    assert "ratsnest crossings" in r.stdout
    assert dst.read_text() == PINS_5C.read_text()          # --report writes nothing
    r = subprocess.run([sys.executable, str(ROOT / "hw/tools/plan_buses.py"), "--write",
                        "--board", str(BOARD), "--pins", str(dst), "--module", str(MODULE)],
                       capture_output=True, text=True, cwd=ROOT)
    assert r.returncode == 0, r.stdout + r.stderr
    assert dst.read_text() == PINS.read_text()
