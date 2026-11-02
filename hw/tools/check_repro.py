# SPDX-License-Identifier: GPL-3.0-or-later
"""hw-repro: `route` lays the committed board's copper again, track for track (Task 5h).

    python3 hw/tools/check_repro.py BOARD.kicad_pcb [--passes N]      # HW REPRO OK
    python3 hw/tools/check_repro.py --selftest BOARD.kicad_pcb         # HW REPRO SELFTEST OK

Every step of route.py is deterministic: the plane fanout, bus_route's rivers and power paths,
the A* first pass and finisher, and Freerouting 2.5.0, whose session is the same byte for byte
on every run. So routing the committed board again must give its copper. route starts by taking
every track and via away, so the routed board is itself the start: a temporary copy of it (with
its .kicad_pro and .kicad_dru) is routed as the justfile's `route` routes it (route.py with
ROUTE_ARGS and the same passes), and the copy's copper signature is compared with BOARD's: every
track (net, layer, ends, width; an arc's middle too) and via (net, place, size, drill), each with
whether it is locked, sorted (bus_route.signature), line for line with duplicates counted. A
route that leaves nets unrouted is compared all the same. It takes as long as a route, so it is the justfile's `repro` recipe, not part of `check`
or `nix flake check`.

The selftest plants differences on BOARD as loaded (one track end moved by 1 um, one via taken
away, one track made wider, one stray via added, one track's lock flipped) and compares each
with BOARD; each must be reported, naming its net. BOARD must have copper (a routed board).
"""
import argparse
import shutil
from collections import Counter
import sys
import tempfile
from pathlib import Path

import pcbnew

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from hw.tools import bus_route, route

# route.py's flags in the justfile's `route` recipe, after the board and --passes (a test
# keeps the two the same).
ROUTE_ARGS = ["--first", "--repair"]
SHOWN = 10                        # differences printed, at most, each way


def signature(board) -> list[str]:
    return bus_route.signature(board).splitlines()


def compare(ours: list[str], again: list[str]) -> list[str]:
    """The lines only in BOARD's signature and only in the re-route's, as problems; counted, so
    a track laid twice is a difference."""
    a, b = Counter(ours), Counter(again)
    out = [f"only in BOARD: {line}" for line in sorted((a - b).elements())]
    out += [f"only in the re-route: {line}" for line in sorted((b - a).elements())]
    return out


def _copy(board: Path, into: Path) -> Path:
    for suffix in (".kicad_pcb", ".kicad_pro", ".kicad_dru"):
        src = board.with_suffix(suffix)
        if src.exists():
            shutil.copyfile(src, into / f"carrier{suffix}")
    return into / "carrier.kicad_pcb"


def _route(copy: Path, passes: int) -> None:
    route.main([str(copy), "--passes", str(passes), *ROUTE_ARGS])


def check(board: Path, passes: int, run=_route) -> list[str]:
    """Route a copy of `board` (`run(copy, passes)`; route.py by default) and compare."""
    board = Path(board)
    with tempfile.TemporaryDirectory(prefix="hw-repro-") as tmp:
        copy = _copy(board, Path(tmp))
        try:
            run(copy, passes)
        except SystemExit as e:          # route lists what it left: the copper still compares
            print(f"route: {e}")
        return compare(signature(pcbnew.LoadBoard(str(board))),
                       signature(pcbnew.LoadBoard(str(copy))))


# --- the selftest ---------------------------------------------------------------------------

def _first(board, via: bool):
    items = sorted((t for t in board.GetTracks() if (t.GetClass() == "PCB_VIA") == via),
                   key=lambda t: bus_route.signature_line(t))
    return items[0] if items else None


def _nudge(b):
    t = _first(b, via=False)
    t.SetEnd(t.GetEnd() + pcbnew.VECTOR2I(1000, 0))
    return t.GetNetname()


def _no_via(b):
    t = _first(b, via=True)
    name = t.GetNetname()
    b.RemoveNative(t)
    return name


def _wider(b):
    t = _first(b, via=False)
    t.SetWidth(t.GetWidth() + pcbnew.FromMM(0.05))
    return t.GetNetname()


def _stray(b):
    t = _first(b, via=True)
    x = pcbnew.PCB_VIA(b)
    x.SetPosition(t.GetPosition() + pcbnew.VECTOR2I(pcbnew.FromMM(5), 0))
    x.SetViaType(pcbnew.VIATYPE_THROUGH)
    x.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
    x.SetWidth(t.GetWidth(pcbnew.F_Cu))
    x.SetDrill(t.GetDrill())
    x.SetNet(t.GetNet())
    b.Add(x)
    return t.GetNetname()


def _lock_flipped(b):
    t = _first(b, via=False)
    t.SetLocked(not t.IsLocked())
    return t.GetNetname()


PLANT = {"a track end moved 1 um": _nudge, "a via taken away": _no_via,
         "a track made wider": _wider, "a stray via": _stray,
         "a track's lock flipped": _lock_flipped}


def selftest(board: Path) -> list[str]:
    """Each plant must show as a difference naming its net; returns the plants that did not."""
    ours = signature(pcbnew.LoadBoard(str(board)))
    if not any(line.startswith("track") for line in ours) or \
            not any(line.startswith("via") for line in ours):
        return [f"{board} has no tracks or no vias: route it first"]
    missed = []
    if compare(ours, ours):
        missed.append("the board differs from itself")
    for name, plant in PLANT.items():
        b = pcbnew.LoadBoard(str(board))
        net = plant(b)
        problems = compare(ours, signature(b))
        if not problems or not all(f" {net} " in p for p in problems):
            missed.append(f"{name}: {problems[:2]}")
    return missed


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("board", type=Path)
    ap.add_argument("--passes", type=int, default=20)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        missed = selftest(a.board)
        for m in missed:
            print(f"selftest: not caught: {m}")
        if missed:
            return 1
        print("HW REPRO SELFTEST OK")
        return 0
    problems = check(a.board, a.passes)
    if problems:
        print(f"the re-route differs from {a.board} in {len(problems)} lines:")
        for p in problems[:SHOWN]:
            print(f"  {p}")
        return 1
    n = len(signature(pcbnew.LoadBoard(str(a.board))))
    print(f"the re-route lays the same {n} tracks and vias")
    print("HW REPRO OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
