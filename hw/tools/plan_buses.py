# SPDX-License-Identifier: GPL-3.0-or-later
"""Plan the carrier's FPGA-side buses as uncrossed rivers: which SODIMM contact each bit takes.

    plan_buses.py --report [--board B.kicad_pcb] [--pins pins.toml] [--module M.toml]
    plan_buses.py --write  [...]            (rewrites pins.toml)

Seven auto-routing rounds left the band between the translators and the module socket J2 open
(plan 1b, amendment 2), so the buses are planned the way they are routed by hand, and laid by
hw/tools/bus_route.py from this plan. J2 has two pad rows (8.2 mm apart, centre to centre): the
FRONT row faces the translators, the REAR row the empty area under the module (the highway).
A net reaches its pad one of three ways, and each way is one copper layer at the pad:

* front, from below, on F.Cu: the river runs up from its translator on F.Cu into the front pads;
* rear, from below, on B.Cu: a via at the translator, B.Cu under the front row, a via in the
  6.2 mm gap between the rows, and a short F.Cu stub up into the rear pad;
* rear, from above: up the free B.Cu under J2's unused left end into the highway, across, and
  down into the rear pad on F.Cu.

A river is a group of nets that leave one part side by side (RIVERS). On one layer two nets of a
river cross unless the order of their pads along J2 is the order of their sources, so the plan
is an order-preserving (monotone) assignment: every river's pads ascend with its sources, the
rivers that share a row follow each other along it in a fixed order (ROWS), and among all such
assignments the one with the least lateral travel (sum of |pad x - source x|) is taken. That is
the river-routing dynamic programme over ordered matchings (Leiserson and Pinter, "Optimal
placement for river routing", SIAM J. Comput. 12(3), 1983): f[i][j] = min(f[i][j-1],
f[i-1][j-1] + |x_j - s_i|), O(sources x pads), optimal. The rest group (single nets that
bus_route leaves to Freerouting) takes the nearest of the pads left over, in either row.

Ground, the rails and the module-only balls are untouched; every contact is a PIO of a 3.3 V
bank on the SODIMM (hw-pinmap's rules), used once. assign_pins.crossings, the ratsnest crossing
count, is still printed: straight pad-to-pad lines are not the route any more, so it is a
metric, not the objective.

Run it after the placement changes (its input is where the pads are), then `just -f hw/justfile
board` (J2's pads change nets) and the gates. A test checks that pins.toml is this plan.
"""
import argparse
import copy
import sys
from dataclasses import dataclass
from pathlib import Path

import pcbnew

if __package__ in (None, ""):          # run as a script: put the repo root on the path
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from hw.tools import assign_pins
from hw.tools import module as modmod
from hw.tools import pins as pinsmod

REPO = Path(__file__).resolve().parents[2]
CARRIER = REPO / "hw" / "carrier"
SODIMM = "J2"
FRONT, REAR, ANY = "front", "rear", "any"
BELOW_F, BELOW_B, ABOVE, FREE = "below-F", "below-B", "above", "free"


@dataclass(frozen=True)
class River:
    """Nets that leave `ref` side by side. `row`/`access`: how they reach J2 (module doc).
    `ref` None: each net's own part (its one pad that is not on J2)."""
    name: str
    ref: str | None
    nets: tuple[str, ...]
    row: str
    access: str
    pick: str = "left"            # which pad is the source if a net has several: left/right


def _bits(port: str, n: int) -> tuple[str, ...]:
    return tuple(f"{port}{i}" for i in range(n))


# The rivers, as bus_route lays them. Within a river the order is the sources' order along x.
RIVERS = (
    # front row, from below on F.Cu, in this order along J2
    River("address", "U2", ("slot_a_oe_n",) + _bits("slot_a", 16), FRONT, BELOW_F),
    # the two clock buffers' 0 ohm links R1, R2 sit in the channel between U2 and U5
    River("clocks_f", None, ("slot_ph0", "slot_ph1"), FRONT, BELOW_F),
    River("control", "U5", ("slot_iosel_n", "slot_devsel_n", "slot_m2sel_n", "slot_m2b0",
                            "slot_sync_n", "slot_iostrb_n", "slot_reset_n"), FRONT, BELOW_F),
    # rear row, in this order along J2: the listen bus from below on B.Cu, then the data
    # translator's side over the highway, then the host header lines (they start up there)
    River("listen", "U4", _bits("slot_dl", 8), REAR, BELOW_B),
    River("data_oe", "U1", ("slot_d_oe_n",), REAR, BELOW_B),
    River("data", "U1", _bits("slot_d", 8) + ("slot_d_dir",), REAR, ABOVE),
    River("host", None, ("uart_tx", "uart_rx", "safe_n"), REAR, ABOVE),
    # R3 and R4 sit under U5: those two clocks leave on B.Cu, under the control river
    River("clocks_b", None, ("slot_q3", "slot_7m"), REAR, BELOW_B),
    # single nets the router (Freerouting) joins: the nearest pads left, either row
    River("rest", None, ("slot_rw", "slot_a_dir", "slot_inh_n_o", "slot_rdy_o",
                         "slot_reset_n_o", "slot_dma_n_o", "slot_irq_n_o", "slot_nmi_n_o"),
          ANY, FREE, pick="right"),
)
ROWS = (FRONT, REAR, ANY)          # planned in this order; ANY takes what is left, beyond them


# --- positions --------------------------------------------------------------------------

Point = tuple[float, float]


def _mm(p) -> Point:
    return (pcbnew.ToMM(p.x), pcbnew.ToMM(p.y))


def sodimm_pads(board) -> dict[int, tuple[float, str]]:
    """J2 contact -> (x in mm, FRONT or REAR). The front row is the one nearer the translators
    (the larger y)."""
    fp = board.FindFootprintByReference(SODIMM)
    pads = {int(p.GetNumber()): _mm(p.GetPosition()) for p in fp.Pads()
            if p.GetNumber().isdigit()}
    ys = sorted({round(y, 3) for _, y in pads.values()})
    front_y = max(ys)
    return {n: (x, FRONT if abs(y - front_y) < 0.5 else REAR) for n, (x, y) in pads.items()}


def source_pad(board, net: str, ref: str | None, pick: str = "left"):
    """The pad a net leaves from: its pad on `ref`, or, with no `ref`, its pad on any part but
    J2; of several, the leftmost (`pick` "left") or the rightmost."""
    pads = [p for fp in board.GetFootprints() for p in fp.Pads()
            if p.GetNetname() == net and fp.GetReference() != SODIMM
            and (ref is None or fp.GetReference() == ref)]
    if not pads:
        raise ValueError(f"plan_buses: net {net} has no pad on {ref or 'any part but J2'}")
    key = lambda p: (p.GetX(), p.GetY())
    return min(pads, key=key) if pick == "left" else max(pads, key=key)


def sources(board, river: River) -> list[tuple[str, float]]:
    """(net, source x) of the river's nets, in the river's order (source x, then name)."""
    out = [(n, pcbnew.ToMM(source_pad(board, n, river.ref, river.pick).GetX()))
           for n in river.nets]
    return sorted(out, key=lambda t: (t[1], t[0]))


# --- the assignment ---------------------------------------------------------------------

def monotone(srcs: list[float], pads: list[tuple[float, int]]) -> list[int]:
    """The order-preserving assignment of `srcs` (in order) to `pads` ((x, contact), sorted by
    x) with the least sum of |pad x - source x|: the contact for each source. Ties go to the
    lower pads, so the result is the same on every run."""
    n, m = len(srcs), len(pads)
    if n > m:
        raise ValueError(f"plan_buses: {n} nets for {m} pads")
    INF = float("inf")
    f = [[INF] * (m + 1) for _ in range(n + 1)]
    for j in range(m + 1):
        f[0][j] = 0.0
    for i in range(1, n + 1):
        for j in range(i, m + 1):
            take = f[i - 1][j - 1] + abs(pads[j - 1][0] - srcs[i - 1])
            f[i][j] = min(f[i][j - 1], take)
    out, j = [], m
    for i in range(n, 0, -1):
        while j > i and f[i][j] == f[i][j - 1]:    # pad j not needed: take a lower one
            j -= 1
        out.append(pads[j - 1][1])
        j -= 1
    return out[::-1]


def pool(mod: modmod.ModuleMap, pm: pinsmod.PinMap) -> list[int]:
    """SODIMM contacts whose ball is a PIO of a 3.3 V bank (hw-pinmap's rule)."""
    return assign_pins._pool(mod, pm)


def assignment(board, mod: modmod.ModuleMap, pm: pinsmod.PinMap) -> dict[str, int]:
    """The plan: net -> SODIMM contact, for every river net."""
    pads = sodimm_pads(board)
    free = [c for c in pool(mod, pm) if c in pads]
    out: dict[str, int] = {}
    for row in ROWS:
        rivers = [r for r in RIVERS if r.row == row]
        seq = [(n, x) for r in rivers for n, x in sources(board, r)]
        if not seq:
            continue
        if row == ANY:              # beyond every river's pads, so their corridors stay clear
            edge = max(pads[c][0] for c in out.values())
            cand = sorted((pads[c][0], c) for c in free if pads[c][0] > edge)
        else:
            cand = sorted((pads[c][0], c) for c in free if pads[c][1] == row)
        got = monotone([x for _, x in seq], cand)
        for (net, _), c in zip(seq, got):
            out[net] = c
        free = [c for c in free if c not in got]
    return out


def plan(board, mod: modmod.ModuleMap, pm: pinsmod.PinMap) -> pinsmod.PinMap:
    """A copy of `pm` with every river net's ball on its planned contact."""
    want = assignment(board, mod, pm)
    out = copy.deepcopy(pm)
    for p in out.ports.values():
        if p.on_module:
            continue
        p.balls = [mod.balls[want[net]] for net, _, _ in pinsmod.bits(p)]
    missing = {net for p in out.ports.values() if not p.on_module
               for net, _, _ in pinsmod.bits(p)} - set(want)
    if missing:
        raise ValueError(f"plan_buses: no river for {sorted(missing)}")
    return out


# --- the metric: is each river a river? -------------------------------------------------

def wiring(mod: modmod.ModuleMap, pm: pinsmod.PinMap) -> dict[str, int]:
    """Net -> SODIMM contact, as `pm` wires it."""
    return {net: modmod.pin_for_ball(mod, ball) for p in pm.ports.values()
            if not p.on_module for net, _, ball in pinsmod.bits(p)}


def river_problems(board, mod: modmod.ModuleMap, pm: pinsmod.PinMap) -> list[str]:
    """What keeps `pm` from being planar under the plan's topology: a net on the wrong row for
    its river, or two nets of a river whose pads are in the opposite order to their sources
    (an inversion: on one layer they cross)."""
    pads = sodimm_pads(board)
    have = wiring(mod, pm)
    out = []
    for r in RIVERS:
        seq = sources(board, r)
        for net, _ in seq:
            if r.row != ANY and pads[have[net]][1] != r.row:
                out.append(f"{r.name}: {net} is on the {pads[have[net]][1]} row "
                           f"(contact {have[net]}), the river reaches J2 by the {r.row} row")
        xs = [(net, pads[have[net]][0]) for net, _ in seq]
        for i in range(len(xs)):
            for j in range(i + 1, len(xs)):
                if xs[i][1] > xs[j][1]:
                    out.append(f"{r.name}: {xs[i][0]} and {xs[j][0]} cross "
                               f"(pads at x {xs[i][1]:.2f} and {xs[j][1]:.2f})")
    return out


def travel(board, mod: modmod.ModuleMap, pm: pinsmod.PinMap) -> float:
    """Sum over the river nets of |pad x - source x|, in mm."""
    pads = sodimm_pads(board)
    have = wiring(mod, pm)
    return sum(abs(pads[have[n]][0] - x) for r in RIVERS for n, x in sources(board, r))


# --- pins.toml ---------------------------------------------------------------------------

HEADER = """\
# SPDX-License-Identifier: CERN-OHL-S-2.0
#
# carrier_top's port -> ball assignment (rtl/board/carrier_top.v), for the LFE5U-25F on
# the iCESugar-Pro module. The LPF is generated from this file. hw-pinmap checks that every
# ball is a PIO in a 3.3 V bank, that each ball is on a SODIMM pin (hw/carrier/
# icesugar_pro_v1_3.toml), and that the netlist carries the port's net on that pin.
#
# Generated by hw/tools/plan_buses.py for the placement in hw/carrier/placement.toml: every
# bus is a river (its pads along J2 in the order of its pins on its translator, on the J2 row
# its route reaches), with the least lateral travel; hw/tools/bus_route.py lays it. Do not
# hand-edit: rerun `python3 hw/tools/plan_buses.py --write` after the placement changes, then
# `just -f hw/justfile board`. A test checks that this file is the plan.
# Every bank is 3.3 V (the module powers VCCIO from 3V3), so the bank does not constrain it.

"""


def render(pm: pinsmod.PinMap, mod: modmod.ModuleMap) -> str:
    text = assign_pins.render(pm, mod)
    body = text[text.index('part = "'):]
    return HEADER + body


# --- command line ------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--report", action="store_true", help="print the metrics, change nothing")
    mode.add_argument("--write", action="store_true", help="rewrite pins.toml with the plan")
    ap.add_argument("--board", type=Path, default=CARRIER / "carrier.kicad_pcb")
    ap.add_argument("--pins", type=Path, default=CARRIER / "pins.toml")
    ap.add_argument("--module", type=Path, default=CARRIER / "icesugar_pro_v1_3.toml")
    ap.add_argument("--netlist", type=Path, help="carrier.net (default: built on the fly)")
    a = ap.parse_args(argv)
    board = pcbnew.LoadBoard(str(a.board))
    mod, pm = modmod.load(a.module), pinsmod.load(a.pins)
    new = plan(board, mod, pm)
    nl = assign_pins._netlist(a.netlist)
    for label, p in (("current", pm), ("planned", new)):
        probs = river_problems(board, mod, p)
        print(f"{label}: {len(probs)} river problems, lateral travel "
              f"{travel(board, mod, p):.1f} mm, ratsnest crossings "
              f"{assign_pins.crossings_for(board, nl, mod, p)}")
        for line in probs[:12]:
            print(f"  {line}")
        if len(probs) > 12:
            print(f"  ... and {len(probs) - 12} more")
    if a.write:
        a.pins.write_text(render(new, mod))
        print(f"wrote {a.pins}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
