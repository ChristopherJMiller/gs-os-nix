# SPDX-License-Identifier: GPL-3.0-or-later
"""Routing quality numbers for the carrier (the plan-1b audit's measures).

    python3 hw/tools/route_metrics.py BOARD.kicad_pcb

* detour: per net with two or more pads (GND and the other rails excluded: the pours carry
  them), routed track length over the minimum spanning tree of its pad centres; median, p90
  and max, and the worst five nets;
* vias, and how many of them are GND stitching vias;
* GND fill on the back layer under the translator row: the share of the box round U1, U2,
  U4 and U5 that is filled copper of the back GND pour;
* the longest clock run within 0.3 mm (edge to edge) of another signal's track.
"""
import math
import statistics
import sys
from pathlib import Path

import pcbnew

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from hw.tools import route

RAILS = {"GND", "+5V", "+5V_SLOT", "+5V_MOD", "+3V3", "+12V_SLOT", "-5V_SLOT", "-12V_SLOT"}
CLOCKS = route.CLOCK_NETS
ROW = ("U1", "U2", "U4", "U5")
NEAR_MM = 0.3


def _mst(points: list[tuple[float, float]]) -> float:
    if len(points) < 2:
        return 0.0
    seen, total = {0}, 0.0
    dist = {i: math.dist(points[0], points[i]) for i in range(1, len(points))}
    while dist:
        i = min(dist, key=dist.get)
        total += dist.pop(i)
        seen.add(i)
        for j in dist:
            dist[j] = min(dist[j], math.dist(points[i], points[j]))
    return total


def detours(board) -> dict[str, float]:
    pads: dict[str, list] = {}
    for fp in board.GetFootprints():
        for p in fp.Pads():
            if p.GetNetCode():
                pads.setdefault(p.GetNetname(), []).append(
                    (pcbnew.ToMM(p.GetPosition().x), pcbnew.ToMM(p.GetPosition().y)))
    length: dict[str, float] = {}
    for t in board.GetTracks():
        if t.GetClass() != "PCB_VIA":
            length[t.GetNetname()] = length.get(t.GetNetname(), 0.0) + pcbnew.ToMM(t.GetLength())
    out = {}
    for name, pts in pads.items():
        if name in RAILS or len(pts) < 2:
            continue
        ideal = _mst(pts)
        if ideal > 0.5 and name in length:
            out[name] = length[name] / ideal
    return out


def back_gnd_fill(board) -> float:
    boxes = [board.FindFootprintByReference(r).GetBoundingBox() for r in ROW]
    x0, y0 = min(b.GetLeft() for b in boxes), min(b.GetTop() for b in boxes)
    x1, y1 = max(b.GetRight() for b in boxes), max(b.GetBottom() for b in boxes)
    rect = pcbnew.SHAPE_POLY_SET()
    rect.NewOutline()
    for x, y in ((x0, y0), (x1, y0), (x1, y1), (x0, y1)):
        rect.Append(x, y)
    fill = pcbnew.SHAPE_POLY_SET()
    for z in board.Zones():
        if not z.GetIsRuleArea() and z.GetNetname() == "GND" and z.IsOnLayer(pcbnew.B_Cu):
            fill.Append(z.GetFilledPolysList(pcbnew.B_Cu))
    fill.Simplify()
    fill.BooleanIntersection(rect)
    return 100.0 * fill.Area() / rect.Area()


def _seg_dist(a0, a1, b0, b1) -> float:
    return route._seg_dist(a0, a1, b0, b1)


def clock_parallel_mm(board) -> tuple[float, str]:
    """Longest stretch of a clock track with another signal's track within NEAR_MM
    (edge to edge) on the same layer, sampled every 0.1 mm; and where."""
    others = [t for t in board.GetTracks() if t.GetClass() != "PCB_VIA"
              and t.GetNetname() not in RAILS and t.GetNetname() not in CLOCKS]
    best, where = 0.0, ""
    for c in (t for t in board.GetTracks()
              if t.GetClass() != "PCB_VIA" and t.GetNetname() in CLOCKS):
        a, b = c.GetStart(), c.GetEnd()
        n = max(1, int(pcbnew.ToMM(int((b - a).EuclideanNorm())) / 0.1))
        run = 0.0
        for k in range(n + 1):
            p = pcbnew.VECTOR2I(a.x + (b.x - a.x) * k // n, a.y + (b.y - a.y) * k // n)
            near = any(o.GetLayer() == c.GetLayer() and o.GetNetCode() != c.GetNetCode()
                       and _seg_dist(p, p, o.GetStart(), o.GetEnd())
                       - pcbnew.ToMM(o.GetWidth()) / 2 - pcbnew.ToMM(c.GetWidth()) / 2 <= NEAR_MM
                       for o in others)
            run = run + 0.1 if near else 0.0
            if run > best:
                best, where = run, c.GetNetname()
    return best, where


def main(argv: list[str] | None = None) -> None:
    board = pcbnew.LoadBoard(str((argv or sys.argv[1:])[0]))
    route.refill(board)
    d = detours(board)
    vals = sorted(d.values())
    vias = [t for t in board.GetTracks() if t.GetClass() == "PCB_VIA"]
    gnd = [v for v in vias if v.GetNetname() == "GND"]
    print(f"detour over {len(vals)} nets: median {statistics.median(vals):.2f}  "
          f"p90 {vals[int(0.9 * (len(vals) - 1))]:.2f}  max {vals[-1]:.2f}")
    worst = sorted(d, key=d.get, reverse=True)[:5]
    print("  worst:", ", ".join(f"{n} {d[n]:.2f}" for n in worst))
    print(f"vias: {len(vias)} ({len(gnd)} on GND)")
    print(f"B.Cu GND fill under {'/'.join(ROW)}: {back_gnd_fill(board):.0f} %")
    run, name = clock_parallel_mm(board)
    print(f"longest clock run within {NEAR_MM} mm of another signal: {run:.1f} mm ({name})")


if __name__ == "__main__":
    main()
