# SPDX-License-Identifier: GPL-3.0-or-later
"""Re-assign carrier_top's FPGA balls: the bus plan (plan_buses), scored by ratsnest crossings.

    assign_pins.py --report [--board B.kicad_pcb] [--pins pins.toml] [--module M.toml]
    assign_pins.py --write  [...]            (rewrites pins.toml with plan_buses' plan)
    assign_pins.py --write --anneal [...]    (the old ratsnest annealing instead)

Since Task 5f (plan 1b, amendment 2) the assignment is hw/tools/plan_buses.py's: every bus an
order-preserving river onto the J2 row its route reaches, which bus_route lays. This file keeps
the ratsnest crossing count, which --report prints for the current and the planned assignment
(a metric now, not the objective), and the annealing search it used before (--anneal).

The carrier routes its signals on 2 layers (F.Cu and B.Cu; the inner two are planes), so
every crossing of two ratsnest lines is a via, a detour or a failed route. The FPGA module's pins can be wired to any of the SODIMM's PIO contacts, so
which ball carries which bit is free; this chooses it for a given placement (the board
file) by minimising the number of pairs of straight pad-to-pad segments that cross, each
net being its minimum spanning tree. Ground and the rails are planes and are not counted.

`crossings` scores a board as wired; `crossings_for` scores it with a pin map's balls on
their SODIMM contacts; `propose` searches. The search is a seeded simulated annealing over
the balls of the bits that are wired to J2, moving a bit to a free contact or swapping two.
It keeps pins.toml's rules (every ball a PIO of a 3.3 V bank, on the SODIMM, used once;
bits on the module stay) and then puts every multi-bit bus in order along the socket.

Run it after the placement changes (its input is where the pads are), then `just -f
hw/justfile netlist` for the gates.
"""
import argparse
import copy
import math
import random
import subprocess
import sys
import tempfile
from pathlib import Path

import pcbnew

if __package__ in (None, ""):          # run as a script: put the repo root on the path
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from hw.tools import build_board as bb
from hw.tools import check_pinmap
from hw.tools import module as modmod
from hw.tools import netlist as nlmod
from hw.tools import pins as pinsmod

REPO = Path(__file__).resolve().parents[2]
CARRIER = REPO / "hw" / "carrier"
SODIMM = "J2"
SKIP_NETS = set(bb.POWER_NETS)     # planes and wide rails: not counted
Point = tuple[float, float]
Seg = tuple[Point, Point]


# --- geometry -----------------------------------------------------------------------

def pad_positions(board) -> dict[tuple[str, str], Point]:
    """(reference, pad number) -> pad centre in mm. Pads with no number are left out."""
    pos = {}
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            if pad.GetNumber():
                pos[(fp.GetReference(), pad.GetNumber())] = (
                    pcbnew.ToMM(pad.GetX()), pcbnew.ToMM(pad.GetY()))
    return pos


def mst(points: list[Point]) -> list[Seg]:
    """Minimum spanning tree of `points` (Prim, Euclidean): the segments."""
    if len(points) < 2:
        return []
    done = [points[0]]
    rest = list(points[1:])
    segs: list[Seg] = []
    while rest:
        a, b = min(((p, q) for p in done for q in rest), key=lambda e: math.dist(*e))
        segs.append((a, b))
        done.append(b)
        rest.remove(b)
    return segs


def _side(a: Point, b: Point, c: Point) -> float:
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])


def cross(s: Seg, t: Seg) -> bool:
    """True if the two segments properly cross (not merely touching at an end)."""
    (a, b), (c, d) = s, t
    if (max(a[0], b[0]) < min(c[0], d[0]) or max(c[0], d[0]) < min(a[0], b[0])
            or max(a[1], b[1]) < min(c[1], d[1]) or max(c[1], d[1]) < min(a[1], b[1])):
        return False
    d1, d2 = _side(a, b, c), _side(a, b, d)
    d3, d4 = _side(c, d, a), _side(c, d, b)
    return d1 * d2 < 0 and d3 * d4 < 0


def _net_segments(nl: nlmod.Netlist, pos: dict[tuple[str, str], Point],
                  wiring: dict[str, int] | None = None) -> dict[str, list[Seg]]:
    """Net -> its tree's segments. `wiring` puts the named nets on the given SODIMM
    contacts, whatever the netlist says."""
    out: dict[str, list[Seg]] = {}
    for net, nodes in nl.nets.items():
        if not net or net in SKIP_NETS:
            continue
        if wiring and net in wiring:
            nodes = {n for n in nodes if n[0] != SODIMM} | {(SODIMM, str(wiring[net]))}
        pts = sorted({pos[n] for n in nodes if n in pos})
        segs = mst(pts)
        if segs:
            out[net] = segs
    return out


def _count(segs_by_net: dict[str, list[Seg]]) -> int:
    flat = [(net, s) for net, segs in segs_by_net.items() for s in segs]
    n = 0
    for i, (na, sa) in enumerate(flat):
        for nb, sb in flat[i + 1:]:
            if na != nb and cross(sa, sb):
                n += 1
    return n


def crossings(board, nl: nlmod.Netlist) -> int:
    """Pairs of ratsnest segments (each net a minimum spanning tree of its pads) that cross,
    for the board as the netlist wires it."""
    return _count(_net_segments(nl, pad_positions(board)))


def _wiring(mod: modmod.ModuleMap, pm: pinsmod.PinMap) -> dict[str, int]:
    """Bit net -> SODIMM contact, for every bit that is wired to J2."""
    return {net: modmod.pin_for_ball(mod, ball) for p in pm.ports.values()
            if not p.on_module for net, _, ball in pinsmod.bits(p)}


def crossings_for(board, nl: nlmod.Netlist, mod: modmod.ModuleMap,
                  pm: pinsmod.PinMap) -> int:
    """`crossings`, with the bits of `pm` on the SODIMM contacts its balls name."""
    return _count(_net_segments(nl, pad_positions(board), _wiring(mod, pm)))


# --- the search ---------------------------------------------------------------------

def _pool(mod: modmod.ModuleMap, pm: pinsmod.PinMap) -> list[int]:
    """SODIMM contacts whose ball is a PIO in a 3.3 V bank."""
    tre = check_pinmap.Trellis(pm.part)
    return sorted(pin for pin, ball in mod.balls.items()
                  if ball in tre.bank and mod.bank_vccio.get(tre.bank[ball]) == 3.3)


def propose(board, nl: nlmod.Netlist, mod: modmod.ModuleMap, pm: pinsmod.PinMap, *,
            seed: int = 1, iters: int = 20000, starts: int = 6) -> pinsmod.PinMap:
    """A copy of `pm` with the bits' balls re-assigned to cut ratsnest crossings: the best of
    `starts` annealing runs of `iters` moves (seeds `seed`, `seed` + 1, ...)."""
    pos = pad_positions(board)
    bits = [(p.name, i, net) for p in pm.ports.values() if not p.on_module
            for i, (net, _, _) in enumerate(pinsmod.bits(p))]
    nets = [net for _, _, net in bits]
    wiring = _wiring(mod, pm)
    pool = _pool(mod, pm)
    if not set(wiring.values()) <= set(pool):
        raise ValueError("pins.toml uses a ball that is not a 3.3 V PIO on the SODIMM")
    base = _net_segments(nl, pos, wiring)
    fixed = [(n, s) for n, segs in base.items() if n not in wiring for s in segs]
    nodes_of = {net: {n for n in nl.nets[net] if n[0] != SODIMM} for net in nets}
    pad_of = {pin: pos[(SODIMM, str(pin))] for pin in pool}

    def segs(net: str, pin: int) -> list[Seg]:
        return mst(sorted({pos[n] for n in nodes_of[net] if n in pos} | {pad_of[pin]}))

    fixed_x = sum(1 for i, (na, sa) in enumerate(fixed) for nb, sb in fixed[i + 1:]
                  if na != nb and cross(sa, sb))

    def anneal(seed: int) -> tuple[int, dict[str, int]]:
        state = dict(wiring)
        cur = {net: segs(net, state[net]) for net in nets}

        def x_of(mine: list[Seg], skip: set[str]) -> int:
            """Crossings of `mine` with every other net's segments but those of `skip`."""
            n = 0
            for _, t in fixed:
                for s in mine:
                    if cross(s, t):
                        n += 1
            for other in nets:
                if other in skip:
                    continue
                for t in cur[other]:
                    for s in mine:
                        if cross(s, t):
                            n += 1
            return n

        rng = random.Random(seed)
        free = [p for p in pool if p not in set(state.values())]
        total = sum(x_of(cur[n], {n}) for n in nets) // 2 + fixed_x
        best_total, best = total, dict(state)
        t0, t1 = 1.5, 0.05
        for it in range(iters):
            temp = t0 * (t1 / t0) ** (it / iters)
            a = rng.choice(nets)
            if free and rng.random() < 0.5:                 # move to a free contact
                k = rng.randrange(len(free))
                new_pin = free[k]
                old = x_of(cur[a], {a})
                new_segs = segs(a, new_pin)
                new = x_of(new_segs, {a})
                if new <= old or rng.random() < math.exp((old - new) / temp):
                    free[k] = state[a]
                    state[a], cur[a] = new_pin, new_segs
                    total += new - old
            else:                                           # swap two bits' contacts
                b = rng.choice(nets)
                if a == b:
                    continue
                old = x_of(cur[a], {a}) + x_of(cur[b], {a, b})
                sa_, sb_ = segs(a, state[b]), segs(b, state[a])
                saved = cur[a], cur[b]
                cur[a], cur[b] = sa_, sb_
                new = x_of(sa_, {a}) + x_of(sb_, {a, b})
                if new <= old or rng.random() < math.exp((old - new) / temp):
                    state[a], state[b] = state[b], state[a]
                    total += new - old
                else:
                    cur[a], cur[b] = saved
            if total < best_total:
                best_total, best = total, dict(state)
        return best_total, best

    _, best = min((anneal(seed + k) for k in range(starts)), key=lambda r: r[0])

    def in_bus_order(state: dict[str, int]) -> dict[str, int]:
        """Every multi-bit bus in order along the socket: keep the contacts it chose, deal
        them to the bits in ascending or descending order, whichever crosses less."""
        final = dict(state)
        for p in pm.ports.values():
            if p.on_module or len(p.balls) < 2:
                continue
            bit_nets = [net for net, _, _ in pinsmod.bits(p)]
            chosen = sorted(final[n] for n in bit_nets)
            options = []
            for order in (chosen, chosen[::-1]):
                trial = dict(final)
                trial.update(zip(bit_nets, order))
                options.append((_count(_net_segments(nl, pos, trial)), order))
            final.update(zip(bit_nets, min(options, key=lambda o: o[0])[1]))
        return final

    # the search's best, put in bus order, against the start put in bus order: never worse
    final = min((in_bus_order(best), in_bus_order(wiring)),
                key=lambda st: _count(_net_segments(nl, pos, st)))

    out = copy.deepcopy(pm)
    for p in out.ports.values():
        if not p.on_module:
            p.balls = [mod.balls[final[net]] for net, _, _ in pinsmod.bits(p)]
    return out


# --- pins.toml ---------------------------------------------------------------------

HEADER = """\
# SPDX-License-Identifier: CERN-OHL-S-2.0
#
# carrier_top's port -> ball assignment (rtl/board/carrier_top.v), for the LFE5U-25F on
# the iCESugar-Pro module. The LPF is generated from this file. hw-pinmap checks that every
# ball is a PIO in a 3.3 V bank, that each ball is on a SODIMM pin (hw/carrier/
# icesugar_pro_v1_3.toml), and that the netlist carries the port's net on that pin.
#
# The balls are chosen by hw/tools/assign_pins.py for the placement in hw/carrier/
# placement.toml: it moves each bit to the SODIMM contact that leaves the fewest ratsnest
# crossings on the 2-layer board, keeping every bus in bit order along the socket. Do not
# hand-edit; rerun `python3 hw/tools/assign_pins.py --write` after the placement changes.
# Every bank is 3.3 V (the module powers VCCIO from 3V3), so the bank does not constrain it.
#
# Every one of the slot-side bits is a slot signal, so there is no pin-end preference to
# honour: the socket's orientation against J1 is the placement task's choice.

"""


def render(pm: pinsmod.PinMap, mod: modmod.ModuleMap) -> str:
    std = next(iter(pm.ports.values())).io_standard
    out = [HEADER.rstrip("\n"), "", f'part = "{pm.part}"', f'io_standard = "{std}"']
    for p in pm.ports.values():
        out.append(f"\n[ports.{p.name}]")
        out.append(f'dir = "{p.dir}"')
        if p.on_module:
            out.append("on_module = true")
        if p.io_standard != std:
            out.append(f'io_standard = "{p.io_standard}"')
        balls = ", ".join(f'"{b}"' for b in p.balls)
        if p.on_module:
            out.append(f"balls = [{balls}]")
            continue
        sod = [modmod.pin_for_ball(mod, b) for b in p.balls]
        if len(sod) == 1:
            out.append(f"balls = [{balls}]   # pin {sod[0]}")
        else:
            out.append("# SODIMM pins " + " ".join(str(s) for s in sod))
            out.append(f"balls = [{balls}]")
    return "\n".join(out) + "\n"


# --- command line ------------------------------------------------------------------

def _netlist(path: Path | None) -> nlmod.Netlist:
    if path:
        return nlmod.load(path)
    with tempfile.TemporaryDirectory() as d:
        subprocess.run([sys.executable, "-m", "hw.carrier.top", "--out", d, "--no-schematic"],
                       cwd=REPO, check=True, capture_output=True)
        return nlmod.load(Path(d) / "carrier.net")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--report", action="store_true", help="print current and proposed counts")
    mode.add_argument("--write", action="store_true", help="rewrite pins.toml")
    ap.add_argument("--anneal", action="store_true",
                    help="propose by the ratsnest annealing (--seed/--iters/--starts), not the "
                    "bus plan")
    ap.add_argument("--board", type=Path, default=CARRIER / "carrier.kicad_pcb")
    ap.add_argument("--pins", type=Path, default=CARRIER / "pins.toml")
    ap.add_argument("--module", type=Path, default=CARRIER / "icesugar_pro_v1_3.toml")
    ap.add_argument("--netlist", type=Path, help="carrier.net (default: built on the fly)")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--iters", type=int, default=20000)
    ap.add_argument("--starts", type=int, default=6)
    a = ap.parse_args(argv)
    board = pcbnew.LoadBoard(str(a.board))
    nl = _netlist(a.netlist)
    mod, pm = modmod.load(a.module), pinsmod.load(a.pins)
    from hw.tools import plan_buses
    if a.anneal:
        new = propose(board, nl, mod, pm, seed=a.seed, iters=a.iters, starts=a.starts)
    else:
        new = plan_buses.plan(board, mod, pm)
    before, after = crossings_for(board, nl, mod, pm), crossings_for(board, nl, mod, new)
    print(f"current crossings: {before}")
    print(f"proposed crossings: {after}")
    print(f"river problems: current {len(plan_buses.river_problems(board, mod, pm))}, "
          f"proposed {len(plan_buses.river_problems(board, mod, new))}")
    if a.write:
        a.pins.write_text(render(new, mod) if a.anneal else plan_buses.render(new, mod))
        print(f"wrote {a.pins}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
