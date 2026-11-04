# SPDX-License-Identifier: GPL-3.0-or-later
"""Lay the carrier's buses as planned rivers, deterministically, and lock them (Task 5g).

    python3 hw/tools/bus_route.py BOARD.kicad_pcb [--out OUT.kicad_pcb]

Freerouting could not close the band between the translators and the module socket J2 in
seven rounds; a net-by-net router does not plan a 32 x 80 mm strip globally. Practitioners lay
such buses by hand as ordered, uncrossed rivers, break the fine-pitch connector out on fixed
via positions and leave only the sparse remainder to the autorouter (plan 1b, amendment 2).
This does that, for the plan in hw/tools/plan_buses.py (which J2 contact each bit takes):

* every track is built from straight and 45-degree segments at computed offsets: rows by a
  left-edge assignment with nesting, sideways lanes past obstacles by an exact dynamic programme
  on a 0.01 mm grid, ties broken by position. Nothing is random, so the same board gives the
  same copper, byte for byte;
* every track and via is checked against the board before it is kept (the 0.15/0.15 rules, the
  Clock class's 0.3 mm with KiCad's pad and J2_ESCAPE exemptions, the 0.5 mm edge, hole
  clearances, the bevel keepout, no via in the finger tab). A net that does not fit is not
  forced: nothing of it is laid, and it is reported with the first obstacle, for Freerouting;
* a P19 plane via (and its neck) in a river's way is moved to another spot within 1 mm of its
  pad, clear of everything; a pad that finds no spot keeps its via, and the river nets that
  needed the room are reported instead;
* everything laid is locked, so Freerouting keeps it as fixed wiring (`(type fix)`);
* the clocks are its too (the brief's second option): after the rivers, the Clock class nets
  are joined by route.lay_first (the A* first pass, which keeps the Clock clearance) and locked;
* so are the two 5 V rails (ruling P29, POWER): +5V_MOD round the board's edges and through the
  highway, +5V into U2 by vias under its body, in pieces at the Power width with necks (R23,
  R26); what the pieces leave apart is listed as left, for Freerouting, which routes it with
  every other net it is handed (in no order of ours).

The rivers (carrier_plan(): plan_buses' rivers, each with its layout here; built from the pads):

FPGA side
  data      U1 slot_d0-7, d_dir: up out of U1 into the channel under U4 on F.Cu, left along U4's
            side and C14's, a nested fan onto a row of vias under J2's front row, B.Cu straight
            up J2's unused left end into the highway, across on rows 0.6 mm apart, a via, and
            down into the rear pad on F.Cu.
  listen    U4 slot_dl0-7: a stub and a staggered via above U4, B.Cu at 45 degrees under the
            front row, a staggered via in the 6.2 mm gap, a stub up into the rear pad.
  address   U2 slot_a_oe_n, a0-15: F.Cu straight up, round C8 and C9 by lanes, a common 45
            degree jog, up into the front pads.
  control   U5's seven: the same, into the front pads above it.
Slot side
  slot_data    J1 D0-7 (back fingers): B.Cu up, nested rows under U1, a via under U1's body to
               its pin, on under the channel at 45 degrees to a via under U4's body to its pin.
  slot_address J1 A0-15 (front fingers): F.Cu up, nested rows (the outermost turns highest),
               lanes round C10 and C11, into U2's pins.
"""
import argparse
import math
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import pcbnew

if __package__ in (None, ""):          # run as a script: put the repo root on the path
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from hw.tools import check_edge
from hw.tools import plan_buses as pb
from hw.tools import route as rt

F, B = pcbnew.F_Cu, pcbnew.B_Cu
CU = (F, B)
VIA_MM, DRILL_MM = rt.VIA_MM, rt.VIA_DRILL_MM          # 0.6 / 0.3 (P21)
MARGIN = 0.02            # kept over every clearance (pad polygons are approximations)
PITCH = 0.32             # track centre to centre, 0.15 mm tracks: 0.15 + 0.15 + MARGIN
VIA_PITCH = 0.77         # via centre to centre, 0.6 mm vias: 0.6 + 0.15 + MARGIN
LANE_GAP = 0.46          # lanes' centre to centre: parallel 45-degree tracks stay PITCH apart
VIA_TRACK = VIA_MM / 2 + 0.15 + 0.075 + MARGIN    # via centre to a 0.15 mm track's centre
TOL = 1e-6
P = tuple[float, float]


# --- geometry -------------------------------------------------------------------------------

def _pt_seg(p: P, a: P, b: P) -> float:
    dx, dy = b[0] - a[0], b[1] - a[1]
    l2 = dx * dx + dy * dy
    t = 0.0 if l2 == 0 else max(0.0, min(1.0, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / l2))
    return math.hypot(p[0] - a[0] - t * dx, p[1] - a[1] - t * dy)


def _cross(o: P, a: P, b: P) -> float:
    return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])


def seg_seg(a: P, b: P, c: P, d: P) -> float:
    """Distance between segments ab and cd (0 if they cross)."""
    if (_cross(a, b, c) * _cross(a, b, d) < 0) and (_cross(c, d, a) * _cross(c, d, b) < 0):
        return 0.0
    return min(_pt_seg(a, c, d), _pt_seg(b, c, d), _pt_seg(c, a, b), _pt_seg(d, a, b))


def inside(p: P, poly: list[P]) -> bool:
    n, hit = len(poly), False
    for i in range(n):
        (x0, y0), (x1, y1) = poly[i], poly[(i + 1) % n]
        if (y0 > p[1]) != (y1 > p[1]) and p[0] < x0 + (p[1] - y0) * (x1 - x0) / (y1 - y0):
            hit = not hit
    return hit


def seg_poly(a: P, b: P, poly: list[P]) -> float:
    """Distance from segment ab to a closed polygon (0 if it touches or is inside)."""
    if inside(a, poly) or inside(b, poly):
        return 0.0
    return min(seg_seg(a, b, poly[i], poly[(i + 1) % len(poly)]) for i in range(len(poly)))


@dataclass(frozen=True)
class Seg:
    layer: int
    a: P
    b: P
    w: float


@dataclass(frozen=True)
class Via:
    p: P


def _shape(item) -> tuple[P, P, float, frozenset]:
    """(a, b, radius, layers) of a track or via as a capsule."""
    if isinstance(item, Via):
        return item.p, item.p, VIA_MM / 2, frozenset(CU)
    return item.a, item.b, item.w / 2, frozenset((item.layer,))


# --- what is on the board -------------------------------------------------------------------

@dataclass
class Obs:
    net: str
    layers: frozenset
    kind: str                 # "cap" (a, b, r) | "poly" (pts,) | "hole" (c, r)
    geom: tuple
    label: str
    pad: bool = False
    soft: tuple | None = None    # the plane pad (ref, number) a P19 via or neck belongs to
    box: tuple = (0, 0, 0, 0)


def _mm(v) -> float:
    return pcbnew.ToMM(v)


def _pt(v) -> P:
    return (_mm(v.x), _mm(v.y))


def pad_poly(pad, layer) -> list[list[P]]:
    poly = pcbnew.SHAPE_POLY_SET()
    pad.TransformShapeToPolygon(poly, layer, 0, 2000, pcbnew.ERROR_OUTSIDE)
    out = []
    for k in range(poly.OutlineCount()):
        o = poly.Outline(k)
        out.append([_pt(o.CPoint(i)) for i in range(o.PointCount())])
    return out


def _box(points: list[P], grow: float = 0.0) -> tuple:
    xs, ys = [p[0] for p in points], [p[1] for p in points]
    return (min(xs) - grow, min(ys) - grow, max(xs) + grow, max(ys) + grow)


class Space:
    """The board as obstacles, and the clearance rules checked against them."""

    CELL = 1.0

    def __init__(self, board):
        self.board = board
        ds = board.GetDesignSettings()
        self.hole_cl = _mm(ds.m_HoleClearance)
        self.hole_hole = _mm(ds.m_HoleToHoleMin)
        self.edge = _mm(ds.m_CopperEdgeClearance)
        self.tab = check_edge.tab_geometry(board)
        self.planes = rt.plane_nets(board)
        self._cl: dict[str, float] = {}
        self._w: dict[str, float] = {}
        self.obs: list[Obs | None] = []
        self.cells: dict[tuple, list[int]] = defaultdict(list)
        outline = pcbnew.SHAPE_POLY_SET()
        board.GetBoardPolygonOutlines(outline, False)
        self.edges: list[tuple[P, P]] = []
        self.outline: list[list[P]] = []
        for k in range(outline.OutlineCount()):
            o = outline.Outline(k)
            pts = [_pt(o.CPoint(i)) for i in range(o.PointCount())]
            self.outline.append(pts)
            self.edges += [(pts[i], pts[(i + 1) % len(pts)]) for i in range(len(pts))]
        self.keepouts, self.escape = [], None
        for z in board.Zones():
            if not z.GetIsRuleArea():
                continue
            o = z.Outline()
            pts = [_pt(o.COutline(0).CPoint(i)) for i in range(o.COutline(0).PointCount())]
            if z.GetZoneName() == "J2_ESCAPE":
                self.escape = pts
            if z.GetDoNotAllowTracks() or z.GetDoNotAllowVias():
                self.keepouts.append((pts, frozenset(L for L in CU if z.IsOnLayer(L)),
                                      z.GetDoNotAllowTracks(), z.GetDoNotAllowVias()))
        for fp in board.GetFootprints():
            for pad in fp.Pads():
                self._add_pad(fp, pad)
        groups = soft_groups(board, self.planes)
        for t in board.GetTracks():
            self.add_item(t.GetNetname(), _item(t), soft=groups.get(t.m_Uuid.AsString()),
                          label=f"{t.GetNetname()} {'via' if isinstance(_item(t), Via) else 'track'}")

    # -- rules --
    def clearance(self, net: str) -> float:
        if net not in self._cl:
            self._cl[net] = rt.net_clearance_mm(self.board, net) if net else rt.CLEARANCE_MM
        return self._cl[net]

    def width(self, net: str) -> float:
        if net not in self._w:
            self._w[net] = rt.net_width_mm(self.board, net)
        return self._w[net]

    def is_clock(self, net: str) -> bool:
        return self.clearance(net) > rt.CLEARANCE_MM + TOL

    def in_escape(self, a: P, b: P, r: float) -> bool:
        return self.escape is not None and seg_poly(a, b, self.escape) <= r

    # -- the index --
    def _put(self, o: Obs) -> int:
        i = len(self.obs)
        self.obs.append(o)
        x0, y0, x1, y1 = o.box
        for cx in range(math.floor(x0 / self.CELL), math.floor(x1 / self.CELL) + 1):
            for cy in range(math.floor(y0 / self.CELL), math.floor(y1 / self.CELL) + 1):
                self.cells[(cx, cy)].append(i)
        return i

    def near(self, box) -> list[int]:
        x0, y0, x1, y1 = box
        seen, out = set(), []
        for cx in range(math.floor(x0 / self.CELL), math.floor(x1 / self.CELL) + 1):
            for cy in range(math.floor(y0 / self.CELL), math.floor(y1 / self.CELL) + 1):
                for i in self.cells.get((cx, cy), ()):
                    if i not in seen and self.obs[i] is not None:
                        seen.add(i)
                        out.append(i)
        return sorted(out)

    def _add_pad(self, fp, pad) -> None:
        net, label = pad.GetNetname(), f"{fp.GetReference()}.{pad.GetNumber()}"
        for layer in CU:
            if pad.IsOnLayer(layer):
                for pts in pad_poly(pad, layer):
                    self._put(Obs(net, frozenset((layer,)), "poly", (pts,), label, pad=True,
                                  box=_box(pts)))
        if pad.HasHole():
            c = _pt(pad.GetPosition())
            r = _mm(max(pad.GetDrillSize().x, pad.GetDrillSize().y)) / 2
            self._put(Obs(net, frozenset(CU), "hole", (c, r), label + " hole", pad=True,
                          box=(c[0] - r, c[1] - r, c[0] + r, c[1] + r)))

    def add_item(self, net: str, item, soft=None, label: str = "") -> list[int]:
        a, b, r, layers = _shape(item)
        ids = [self._put(Obs(net, layers, "cap", (a, b, r), label or net, soft=soft,
                             box=_box([a, b], r)))]
        if isinstance(item, Via):
            ids.append(self._put(Obs(net, frozenset(CU), "hole", (item.p, DRILL_MM / 2),
                                     (label or net) + " hole", soft=soft,
                                     box=_box([item.p], DRILL_MM / 2))))
        return ids

    def remove(self, ids) -> None:
        for i in ids:
            self.obs[i] = None

    # -- the check --
    def check(self, net: str, item) -> tuple[list[str], set]:
        """(hard problems, soft groups hit) of laying `item` on `net`."""
        a, b, r, layers = _shape(item)
        is_via = isinstance(item, Via)
        hard, soft = [], set()
        mine = self.clearance(net)
        esc = self.in_escape(a, b, r)
        # the outline: inside it, and the copper-to-edge clearance from every edge
        if not any(inside(a, poly) for poly in self.outline):
            hard.append(f"outside the board at ({a[0]:.2f}, {a[1]:.2f})")
        for e0, e1 in self.edges:
            if seg_seg(a, b, e0, e1) - r < self.edge + MARGIN - TOL:
                hard.append(f"within {self.edge} mm of the board edge")
                break
        if is_via and self.tab:
            t = self.tab
            if (t["tab_l"] - 0.5 <= a[0] <= t["tab_r"] + 0.5
                    and a[1] > t["body_bottom"] - r - 0.05):
                hard.append(f"a via in the finger tab at ({a[0]:.2f}, {a[1]:.2f})")
        for pts, kl, no_tracks, no_vias in self.keepouts:
            if (no_vias if is_via else no_tracks) and layers & kl and seg_poly(a, b, pts) <= r:
                hard.append("in a keepout")
        grow = r + 0.6
        for i in self.near((min(a[0], b[0]) - grow, min(a[1], b[1]) - grow,
                            max(a[0], b[0]) + grow, max(a[1], b[1]) + grow)):
            o = self.obs[i]
            same = bool(net) and o.net == net
            if o.kind == "hole":
                c, hr = o.geom
                if is_via:                                   # hole to hole, any net
                    need, d = self.hole_hole, math.dist(a, c) - DRILL_MM / 2 - hr
                    if d < need + MARGIN - TOL:
                        (soft.add(o.soft) if o.soft else
                         hard.append(f"hole {d:.3f} mm from {o.label}, needs {need}"))
                        continue
                if same:
                    continue
                need, d = self.hole_cl, _pt_seg(c, a, b) - r - hr
            else:
                if same or not (layers & o.layers):
                    continue
                need = max(mine, self.clearance(o.net))
                if o.pad and (self.is_clock(net) or self.is_clock(o.net)):
                    need = rt.CLEARANCE_MM                   # clock_pad_clearance (.kicad_dru)
                if esc and need > rt.CLEARANCE_MM:
                    oa, ob = (o.geom[0], o.geom[1]) if o.kind == "cap" else (None, None)
                    if (o.kind == "cap" and self.in_escape(oa, ob, o.geom[2])) or \
                            (o.kind == "poly" and any(inside(p, self.escape) for p in o.geom[0])):
                        need = rt.CLEARANCE_MM               # clock_clearance_at_the_socket
                if o.kind == "cap":
                    d = seg_seg(a, b, o.geom[0], o.geom[1]) - r - o.geom[2]
                else:
                    d = seg_poly(a, b, o.geom[0]) - r
            if d < need + MARGIN - TOL:
                if o.soft:
                    soft.add(o.soft)
                else:
                    hard.append(f"{d:.3f} mm from {o.label}, needs {need}")
        return hard, soft


def _item(t):
    if t.GetClass() == "PCB_VIA":
        return Via(_pt(t.GetPosition()))
    return Seg(t.GetLayer(), _pt(t.GetStart()), _pt(t.GetEnd()), _mm(t.GetWidth()))


def soft_groups(board, planes) -> dict[str, tuple]:
    """uuid -> (ref, pad) for the locked plane vias and their necks (the P19 fanout): the
    items bus_route may move, and the pad they serve."""
    pads = [(fp, p) for fp in board.GetFootprints() for p in fp.Pads() if p.GetNetname() in planes]
    tracks = [t for t in board.GetTracks() if t.IsLocked() and t.GetNetname() in planes]
    out = {}
    for v in (t for t in tracks if t.GetClass() == "PCB_VIA"):
        pos = v.GetPosition()
        necks = [t for t in tracks if t.GetClass() != "PCB_VIA" and t.GetNetCode() == v.GetNetCode()
                 and pos in (t.GetStart(), t.GetEnd())]
        key = None
        for n in necks:
            far = n.GetEnd() if n.GetStart() == pos else n.GetStart()
            hit = [(fp, p) for fp, p in pads if p.GetNetCode() == n.GetNetCode() and p.HitTest(far)]
            if hit:
                key = (hit[0][0].GetReference(), hit[0][1].GetNumber())
                break
        if key is None:
            fp, p = min(((fp, p) for fp, p in pads if p.GetNetCode() == v.GetNetCode()),
                        key=lambda fpp: (fpp[1].GetPosition() - pos).EuclideanNorm())
            key = (fp.GetReference(), p.GetNumber())
        for t in [v] + necks:
            out[t.m_Uuid.AsString()] = key
    return out


# --- drawing a route ------------------------------------------------------------------------

class Pen:
    """One net's route, drawn as straight and 45-degree segments, with vias."""

    def __init__(self, p: P, layer: int, w: float):
        self.x, self.y = p
        self.layer, self.w = layer, w
        self.items: list = []

    def to(self, x: float, y: float) -> "Pen":
        if abs(x - self.x) > TOL or abs(y - self.y) > TOL:
            self.items.append(Seg(self.layer, (self.x, self.y), (x, y), self.w))
            self.x, self.y = x, y
        return self

    def vert(self, y: float) -> "Pen":
        return self.to(self.x, y)

    def horiz(self, x: float) -> "Pen":
        return self.to(x, self.y)

    def diag(self, x: float, up: bool = True) -> "Pen":
        """45 degrees to x, up (toward smaller y) or down."""
        d = abs(x - self.x)
        return self.to(x, self.y - d if up else self.y + d)

    def via(self, layer: int) -> "Pen":
        self.items.append(Via((self.x, self.y)))
        self.layer = layer
        return self


def merge(items: list) -> list:
    """Join consecutive collinear segments of one layer and width."""
    out: list = []
    for it in items:
        if out and isinstance(it, Seg) and isinstance(out[-1], Seg):
            p = out[-1]
            if (p.layer == it.layer and abs(p.w - it.w) < TOL and p.b == it.a
                    and abs(_cross(p.a, p.b, it.b)) < 1e-9
                    and (p.b[0] - p.a[0]) * (it.b[0] - it.a[0]) + (p.b[1] - p.a[1]) * (it.b[1] - it.a[1]) > 0):
                out[-1] = Seg(p.layer, p.a, it.b, p.w)
                continue
        out.append(it)
    return out


@dataclass
class Route:
    net: str
    river: str
    items: list


# --- the generic constructions --------------------------------------------------------------

LANE_STEP = 0.01          # the lanes' grid, mm
SKIP_COST = 1000.0        # a net left out costs this much displacement


def lanes(xs: list[float], intervals: list[tuple[float, float, float]], gap: float,
          reach: float = 3.0, forbid: list | None = None) -> list[float | None]:
    """Lateral positions for nets at `xs` (in order) that keep their order and `gap` apart, stay
    out of the blocked intervals (lo, hi, centre) and at most `reach` from their own x, with the
    least total displacement: a dynamic programme over the nets and a LANE_STEP grid (which side
    of an obstacle each net passes is part of the choice). A net that cannot be placed is None
    (it costs SKIP_COST). forbid[i]: (lo, hi) ranges net i may not take. Ties go to the leftmost
    position, so the answer is the same each run."""
    n = len(xs)
    if n == 0:
        return []
    lo_x = min(xs) - reach - gap
    m = int(math.ceil((max(xs) + reach + gap - lo_x) / LANE_STEP)) + 1
    pos = np.round(lo_x + np.arange(m) * LANE_STEP, 6)
    free = np.ones(m, dtype=bool)
    for a, b, _ in intervals:
        free &= ~((pos > a) & (pos < b))
    g = int(math.ceil(gap / LANE_STEP - 1e-9))
    idx = np.arange(m)
    # prev[j]: the least cost with the last net placed at j; none: with none placed yet
    prev, none = np.full(m, np.inf), 0.0
    back = []
    for i, x in enumerate(xs):
        cm = np.minimum.accumulate(prev)                  # min over 0..j, first occurrence
        new_min = prev < np.concatenate(([np.inf], cm[:-1]))
        arg = np.maximum.accumulate(np.where(new_min, idx, -1))
        pre = np.where(cm < none, cm, none)               # ties: "none placed" first
        parg = np.where(cm < none, arg, m)
        k = idx - g
        c = np.where(k >= 0, pre[np.clip(k, 0, m - 1)], none)
        a = np.where(k >= 0, parg[np.clip(k, 0, m - 1)], m)
        ok = free & (np.abs(pos - x) <= reach + TOL)
        for a_, b_ in (forbid[i] if forbid else ()):
            ok &= ~((pos >= a_) & (pos <= b_))
        place = np.where(ok, c + np.abs(pos - x), np.inf)
        skip = prev + SKIP_COST
        take = place <= skip
        cur = np.where(take, place, skip)
        back.append((np.where(take, a, idx), take))
        prev, none = cur, none + SKIP_COST
    out: list[float | None] = [None] * n
    j = int(np.argmin(prev)) if prev.min() < none else m
    for i in range(n - 1, -1, -1):
        a, take = back[i]
        if j == m:
            continue
        if take[j]:
            out[i] = float(pos[j])
        j = int(a[j])
    return out


def blocked(space: Space, layer: int, band: tuple, need: float, skip_nets=(),
            soft: bool = False) -> list[tuple[float, float, float]]:
    """The x-intervals (lo, hi, centre) that obstacles on `layer` block for a track's centre in
    the band (x0, y0, x1, y1), grown by `need`. Soft (movable) obstacles only if `soft`."""
    x0, y0, x1, y1 = band
    out = []
    for i in space.near(band):
        o = space.obs[i]
        if (o.kind == "hole" or o.net in skip_nets or layer not in o.layers
                or (o.soft and not soft)):
            continue
        bx0, by0, bx1, by1 = o.box
        g = need                     # a class that asks more (the Clock class) gets it, pads aside
        if not o.pad:
            g += max(0.0, space.clearance(o.net) - rt.CLEARANCE_MM)
        if by1 < y0 or by0 > y1 or bx1 < x0 - g or bx0 > x1 + g:
            continue
        out.append((bx0 - g, bx1 + g, (bx0 + bx1) / 2))
    return sorted(set(out))


def rows(spans: list[tuple[str, float, float, int]], blocked_at, n_rows: int
         ) -> dict[str, int]:
    """Left-edge track assignment with nesting: spans (net, start x, end x, order key) of nets
    moving the same way along one side; returns net -> row (0 nearest the destination side).
    Two overlapping spans: the one with the smaller order key takes the lower row number (the
    outermost net wraps round the inner ones). blocked_at(row, x0, x1) says an obstacle is there."""
    out: dict[str, int] = {}
    placed: list[tuple[float, float, int, str]] = []
    for net, a, b, _ in sorted(spans, key=lambda s: s[3]):
        lo_, hi_ = min(a, b), max(a, b)
        floor = 0
        for pa, pb, prow, _ in placed:
            if pa < hi_ + PITCH and lo_ < pb + PITCH:
                floor = max(floor, prow + 1)
        r = floor
        while r < n_rows and blocked_at(r, lo_, hi_):
            r += 1
        if r >= n_rows:
            continue
        out[net] = r
        placed.append((lo_, hi_, r, net))
    return out


# --- relocating a P19 plane via ---------------------------------------------------------------

def plane_pad(board, key):
    fp = board.FindFootprintByReference(key[0])
    return fp, next(p for p in fp.Pads() if p.GetNumber() == key[1])


def via_spots(space: Space, pad, net: str, reach: float = rt.PLANE_VIA_REACH_MM,
              step: float = 0.05) -> list[P]:
    """Candidate spots for a plane via of `pad`, nearest first: centre 0.4 to `reach` mm from the
    pad's outline (the P19 rule) and 0.4 mm from the net's other pads."""
    layer = F if pad.IsOnLayer(F) else B
    polys = pad_poly(pad, layer)
    pts = [p for poly in polys for p in poly]
    x0, y0, x1, y1 = _box(pts, reach)
    others = []
    for i in space.near((x0 - 1, y0 - 1, x1 + 1, y1 + 1)):
        o = space.obs[i]
        if o.pad and o.net == net and o.kind == "poly" and o.geom[0] not in polys:
            others.append(o.geom[0])
    nx, ny = int(round((x1 - x0) / step)), int(round((y1 - y0) / step))
    gx, gy = np.meshgrid(np.round(x0 + np.arange(nx + 1) * step, 4),
                         np.round(y0 + np.arange(ny + 1) * step, 4), indexing="ij")
    pts = np.stack([gx.ravel(), gy.ravel()], axis=1)
    d = rt._dist_to_polys(pts, polys)
    d[_inside_any(pts, polys)] = 0.0
    ok = (d >= rt.PLANE_VIA_MIN_MM - TOL) & (d <= reach + TOL)
    if others:
        do = rt._dist_to_polys(pts, others)
        do[_inside_any(pts, others)] = 0.0
        ok &= do >= rt.PLANE_VIA_MIN_MM
    rows_ = sorted((round(float(dd), 9), float(p[1]), float(p[0])) for dd, p in zip(d[ok], pts[ok]))
    return [(x, y) for _, y, x in rows_]


def _inside_any(pts: np.ndarray, polys: list) -> np.ndarray:
    """Even-odd test of every point against each polygon: inside any of them."""
    out = np.zeros(len(pts), dtype=bool)
    x, y = pts[:, 0], pts[:, 1]
    for poly in polys:
        a = np.asarray(poly, dtype=float)
        hit = np.zeros(len(pts), dtype=bool)
        for (x0, y0), (x1, y1) in zip(a, np.roll(a, -1, axis=0)):
            if y0 == y1:
                continue
            cross = ((y0 > y) != (y1 > y)) & (x < x0 + (y - y0) * (x1 - x0) / (y1 - y0))
            hit ^= cross
        out |= hit
    return out


def neck_for(pad, p: P, w: float) -> Seg:
    """A neck from the pad's copper nearest `p` (inset by half the neck) to `p`."""
    c = _pt(pad.GetPosition())
    layer = F if pad.IsOnLayer(F) else B
    pts = [q for poly in pad_poly(pad, layer) for q in poly]
    x0, y0, x1, y1 = _box(pts)
    s = (min(max(p[0], x0 + w / 2 + 0.01), x1 - w / 2 - 0.01),
         min(max(p[1], y0 + w / 2 + 0.01), y1 - w / 2 - 0.01))
    if not pad.HitTest(pcbnew.VECTOR2I(pcbnew.FromMM(s[0]), pcbnew.FromMM(s[1]))):
        s = c
    return Seg(layer, s, p, w)


# --- the router ----------------------------------------------------------------------------

class CannotLay(Exception):
    """A river cannot be laid on this board: a part or pad it needs is missing, it reaches J2
    another way than its layout does, or there is no room. run() reports its nets; any other
    exception is a bug and is raised."""


def require(river: pb.River, row: str, access: str) -> None:
    if (river.row, river.access) != (row, access):
        raise CannotLay(f"the {river.name} layout reaches J2 {row}/{access}; the plan says "
                        f"{river.row}/{river.access}")

@dataclass
class Report:
    laid: dict = field(default_factory=dict)        # river -> [nets]
    left: dict = field(default_factory=dict)        # net -> reason
    moved: list = field(default_factory=list)       # "REF.PAD (x0,y0) -> (x1,y1)"
    clocks_left: list = field(default_factory=list)
    kept: list = field(default_factory=list)        # nets that had copper: left as they were

    def lines(self) -> list[str]:
        out = []
        for river, nets in self.laid.items():
            out.append(f"{river}: laid {len(nets)}: {' '.join(nets)}")
        for net, why in self.left.items():
            out.append(f"left to the router: {net}: {why}")
        out += [f"moved plane via {m}" for m in self.moved]
        if self.kept:
            out.append(f"kept as they were (routed before): {' '.join(sorted(self.kept))}")
        if self.clocks_left:
            out.append(f"clocks left to the router: {' '.join(self.clocks_left)}")
        return out


class Router:
    def __init__(self, board):
        self.board = board
        self.space = Space(board)
        self.routes: dict[str, Route] = {}
        self.ids: dict[str, list[int]] = {}
        self.soft_hits: dict[str, set] = {}
        self.report = Report()
        self.reserved: dict[tuple, tuple] = {}     # plane pad -> the (via, neck) it moves to
        # a net with copper already (a continued route keeps its locked tracks) is left alone
        self.existing = {t.GetNetname() for t in board.GetTracks()
                         if t.GetNetname() not in self.space.planes}

    # -- parts and pads --
    def part(self, ref: str):
        fp = self.board.FindFootprintByReference(ref)
        if fp is None:
            raise CannotLay(f"no {ref} on the board")
        return fp

    def pad(self, net: str, ref: str, pick=None):
        pads = [p for p in self.part(ref).Pads() if p.GetNetname() == net]
        if not pads:
            raise CannotLay(f"{ref} has no pad on {net}")
        return pick(pads) if pick else pads[0]

    def box(self, pad, layer=None) -> tuple:
        layer = layer if layer is not None else (F if pad.IsOnLayer(F) else B)
        return _box([q for poly in pad_poly(pad, layer) for q in poly])

    # -- keeping or refusing a route --
    def offer(self, net: str, river: str, items: list, piece: str | None = None) -> bool:
        """Check a whole net (or one `piece` of it, laid or refused on its own); keep it (and make
        it an obstacle) or report why not."""
        key = f"{net} {piece}" if piece else net
        if net in self.existing:
            self.report.kept.append(key)
            return False
        items = merge(items)
        problems, soft = [], set()
        for it in items:
            h, s = self.space.check(net, it)
            problems += h
            soft |= s
            if problems:
                break
        if problems:
            self.report.left[key] = f"{river}: {problems[0]}"
            return False
        self.routes[key] = Route(net, river, items)
        self.ids[key] = [i for it in items for i in self.space.add_item(net, it, label=net)]
        self.soft_hits[key] = soft
        self.report.laid.setdefault(river, []).append(key)
        return True

    def drop(self, key: str, why: str) -> None:
        r = self.routes.pop(key)
        self.space.remove(self.ids.pop(key))
        self.soft_hits.pop(key, None)
        self.report.laid[r.river].remove(key)
        self.report.left[key] = why

    # -- the P19 vias in the way --
    def relocate(self) -> None:
        """Move every plane via (and neck) that a kept route runs into; if a pad finds no spot,
        it keeps its via and the routes that hit it are dropped."""
        keys = sorted({k for s in self.soft_hits.values() for k in s})
        uuid_of = soft_groups(self.board, self.space.planes)
        for key in keys:
            fp, pad = plane_pad(self.board, key)
            net = pad.GetNetname()
            old = [t for t in self.board.GetTracks() if uuid_of.get(t.m_Uuid.AsString()) == key]
            saved = [(i, o) for i, o in enumerate(self.space.obs)
                     if o is not None and o.soft == key]
            self.space.remove([i for i, _ in saved])
            spot = None
            if key in self.reserved:               # the spot the river was planned round
                spot = fit_spot(self.space, None, pad, net, self.reserved[key][0].p)
            if spot is None:
                for p in via_spots(self.space, pad, net):
                    spot = fit_spot(self.space, None, pad, net, p)
                    if spot:
                        break
            if spot is None:                       # no room: the via stays, its hitters go
                for i, o in saved:
                    self.space.obs[i] = o
                for n in sorted(n for n, s in self.soft_hits.items() if key in s):
                    self.drop(n, f"{self.routes[n].river}: the plane via of "
                                 f"{key[0]}.{key[1]} has no other spot")
                continue
            was = next((_pt(t.GetPosition()) for t in old if t.GetClass() == "PCB_VIA"), None)
            for t in old:
                self.board.RemoveNative(t)
            v, neck = spot
            for it in (neck, v):
                lay_item(self.board, net, it, tent=True)
                self.space.add_item(net, it, label=f"{key[0]}.{key[1]} plane via")
            self.report.moved.append(f"{key[0]}.{key[1]} ({was[0]:.2f}, {was[1]:.2f}) -> "
                                     f"({v.p[0]:.2f}, {v.p[1]:.2f})" if was else f"{key}")

    # -- laying --
    def lay(self) -> int:
        n = 0
        for key in sorted(self.routes):
            for it in self.routes[key].items:
                lay_item(self.board, self.routes[key].net, it)
                n += 1
        return n


def lay_item(board, net: str, item, tent: bool = True) -> None:
    ni = board.FindNet(net)
    if isinstance(item, Via):
        v = pcbnew.PCB_VIA(board)
        v.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(item.p[0]), pcbnew.FromMM(item.p[1])))
        v.SetViaType(pcbnew.VIATYPE_THROUGH)
        v.SetLayerPair(F, B)
        v.SetWidth(pcbnew.FromMM(VIA_MM))
        v.SetDrill(pcbnew.FromMM(DRILL_MM))
        if tent:
            v.SetFrontTentingMode(pcbnew.TENTING_MODE_TENTED)
            v.SetBackTentingMode(pcbnew.TENTING_MODE_TENTED)
        v.SetNet(ni)
        v.SetLocked(True)
        board.Add(v)
        return
    t = pcbnew.PCB_TRACK(board)
    t.SetStart(pcbnew.VECTOR2I(pcbnew.FromMM(item.a[0]), pcbnew.FromMM(item.a[1])))
    t.SetEnd(pcbnew.VECTOR2I(pcbnew.FromMM(item.b[0]), pcbnew.FromMM(item.b[1])))
    t.SetLayer(item.layer)
    t.SetWidth(pcbnew.FromMM(item.w))
    t.SetNet(ni)
    t.SetLocked(True)
    board.Add(t)


def signature_line(t) -> str:
    """One track, arc or via as text: net, layer, ends (an arc's middle too) and width, or net,
    place, size and drill; and whether it is locked."""
    if t.GetClass() == "PCB_VIA":
        return (f"via {t.GetNetname()} {t.GetPosition().x} {t.GetPosition().y} "
                f"{t.GetWidth(F)} {t.GetDrill()} {int(t.IsLocked())}")
    if t.GetClass() == "PCB_ARC":
        m = t.GetMid()
        return (f"arc {t.GetNetname()} {t.GetLayerName()} {t.GetStart().x} {t.GetStart().y} "
                f"{m.x} {m.y} {t.GetEnd().x} {t.GetEnd().y} {t.GetWidth()} "
                f"{int(t.IsLocked())}")
    return (f"track {t.GetNetname()} {t.GetLayerName()} {t.GetStart().x} "
            f"{t.GetStart().y} {t.GetEnd().x} {t.GetEnd().y} {t.GetWidth()} "
            f"{int(t.IsLocked())}")


def signature(board) -> str:
    """Every track and via, as text, sorted: two runs that lay the same copper print the same."""
    return "\n".join(sorted(signature_line(t) for t in board.GetTracks())) + "\n"


# --- movable plane vias near a river ---------------------------------------------------------

def soft_keys(space: Space, band: tuple, layer: int) -> list[tuple]:
    """The plane pads whose P19 via lies in the band."""
    keys = set()
    for i in space.near(band):
        o = space.obs[i]
        if o.soft and o.kind == "cap" and o.geom[0] == o.geom[1] and layer in o.layers:
            x0, y0, x1, y1 = o.box
            if not (y1 < band[1] or y0 > band[3] or x1 < band[0] or x0 > band[2]):
                keys.add(o.soft)
    return sorted(keys)


def original_spot(space: Space, key) -> list[tuple]:
    """The plane via of `key` and its neck where they are now: capsules (a, b, radius)."""
    return [o.geom for o in space.obs if o is not None and o.soft == key and o.kind == "cap"]


def fit_spot(space: Space, key, pad, net: str, p: P) -> tuple | None:
    """(via, neck) for the plane via of `key` at `p`, if both clear everything but the via's own
    present copper; the neck at the widest width P19 allows that fits."""
    hard, soft = space.check(net, Via(p))
    if hard or not soft <= {key}:
        return None
    for w in rt._fan_widths(space.board, net):
        neck = neck_for(pad, p, w)
        if net == "+3V3" and math.dist(neck.a, neck.b) > rt.NECK_MAX_3V3_MM:
            continue
        h, s_ = space.check(net, neck)
        if not h and s_ <= {key}:
            return Via(p), neck
    return None


def _caps(spot) -> list[tuple]:
    """A (via, neck) spot as capsules (a, b, radius)."""
    v, neck = spot
    return [(v.p, v.p, VIA_MM / 2), (neck.a, neck.b, neck.w / 2)]


# --- river shapes -----------------------------------------------------------------------------

def _need(r: "Router", nets) -> float:
    return max(r.space.clearance(n) + r.space.width(n) / 2 for n in nets) + MARGIN


def legal_spots(r: "Router", key) -> list[tuple]:
    """Every (via, neck) the plane via of `key` could move to (vias on a 0.2 mm grid): within
    reach of its pad, clear of everything but its own present copper. Nearest the pad first."""
    _, pad = plane_pad(r.board, key)
    net = pad.GetNetname()
    out = []
    for p in via_spots(r.space, pad, net, step=0.2):
        spot = fit_spot(r.space, key, pad, net, p)
        if spot:
            out.append(spot)
    return out


def choose_lanes(r: "Router", xs: list[float], band: tuple, layer: int, nets, gap: float,
                 reach: float, shape) -> list[float | None]:
    """lanes() through `band`, with every movable plane via in it either left where it is or
    moved to a legal spot: a coordinate descent over the vias, keeping a move only if it places
    more nets or displaces them less. shape(i, x) is net i's copper in the band on lane x (with
    its 45-degree step out of or into its pad); a net whose copper there runs into a via counts
    as not placed. The spots it moved vias to are reserved for relocate()."""
    need = _need(r, nets)
    hard = blocked(r.space, layer, band, need, skip_nets=set(nets))
    keys = soft_keys(r.space, band, layer)
    cands = {k: [None] + legal_spots(r, k) for k in keys}

    memo: dict = {}
    shapes: dict = {}
    solved: dict = {}

    def capsules(i, p):
        """Net i's copper on lane p as capsules with their boxes (cached: lanes are a grid)."""
        k = (i, round(p, 3))
        if k not in shapes:
            out = []
            for it in shape(i, p):
                a, b, rad, _ = _shape(it)
                out.append((a, b, rad, _box([a, b], rad)))
            shapes[k] = out
        return shapes[k]

    def clear(i, p):
        """Is net i's copper on lane p clear of everything fixed (not the movable vias)?"""
        k = (i, round(p, 3))
        if k not in memo:
            memo[k] = all(not r.space.check(nets[i], it)[0] for it in shape(i, p))
        return memo[k]

    def solve(intervals, forbid):
        k = (tuple(intervals), tuple(tuple(f) for f in forbid))
        if k not in solved:
            solved[k] = lanes(xs, list(intervals), gap, reach, forbid)
        return solved[k]

    def caps_of(pick):
        return [c for key, p in pick.items()
                for c in (_caps(p) if p is not None else original_spot(r.space, key))]

    def cost(pick):
        extra = []
        for a, b, rad in caps_of(pick):
            if min(a[1], b[1]) - rad <= band[3] + need and max(a[1], b[1]) + rad >= band[1] - need:
                g = rad + need
                extra.append((round(min(a[0], b[0]) - g, 6), round(max(a[0], b[0]) + g, 6),
                              round((a[0] + b[0]) / 2, 6)))
        forbid = [[] for _ in xs]
        for _ in range(8):             # a lane whose step out of the pad hits something: not
            px = solve(sorted(hard + extra), forbid)                     # that far, that way
            again = False
            for i, p in enumerate(px):
                if p is not None and abs(p - xs[i]) > TOL and not clear(i, p):
                    forbid[i].append((p - TOL, math.inf) if p > xs[i] else (-math.inf, p + TOL))
                    again = True
            if not again:
                break
        caps = [(ca, cb, cr, _box([ca, cb], cr + need)) for ca, cb, cr in caps_of(pick)]
        bad = 0
        for i, p in enumerate(px):
            if p is None:
                bad += 1
                continue
            for a, b, rad, bx in capsules(i, p):
                if any(bx[0] <= cx[2] and cx[0] <= bx[2] and bx[1] <= cx[3] and cx[1] <= bx[3]
                       and seg_seg(a, b, ca, cb) - rad - cr < r.space.clearance(nets[i]) + MARGIN
                       for ca, cb, cr, cx in caps):
                    bad += 1
                    break
        return (bad, round(sum(abs(p - x) for p, x in zip(px, xs) if p is not None), 6)), px

    pick = {k: None for k in keys}
    best, px = cost(pick)
    if not keys:
        return px
    for _ in range(3):
        changed = False
        for key in keys:
            for c in cands[key][1:]:
                trial = {**pick, key: c}
                got, tpx = cost(trial)
                if got < best:
                    best, px, pick, changed = got, tpx, trial, True
        if not changed:
            break
    for key, p in pick.items():
        if p is not None:
            r.reserved[key] = p
    return px


def vriver(r: "Router", name: str, members: list, *, y0: float, layer: int = F,
           gap: float = LANE_GAP) -> None:
    """A river up one layer from a row of pads (out of their top ends) into a row of pads above
    (into their bottom ends): out of each pad, sideways onto lanes that clear the obstacles
    between the pads and `y0`, one common 45-degree jog from `y0` to the destination's x, and
    straight up into it. members: (net, source pad, destination pad)."""
    members = sorted(members, key=lambda m: (_mm(m[1].GetX()), m[0]))
    nets = [n for n, _, _ in members]
    sx = [_mm(sp.GetX()) for _, sp, _ in members]
    ye = min(r.box(sp, layer)[1] for _, sp, _ in members)
    def shape(i, p):
        sp = members[i][1]
        return Pen(_pt(sp.GetPosition()), layer, r.space.width(nets[i])).vert(ye).diag(p) \
            .vert(y0).items
    px = choose_lanes(r, sx, (min(sx) - 2, y0, max(sx) + 2, ye - 0.05), layer, nets, gap,
                      reach=ye - y0, shape=shape)
    for (net, sp, dp), x, p in zip(members, sx, px):
        where = f"{name}: {net}"
        if p is None:
            r.report.left[net] = f"{where}: no lane past the obstacles between its pad and y {y0:.2f}"
            continue
        pen = Pen(_pt(sp.GetPosition()), layer, r.space.width(net)).vert(ye).diag(p)
        if pen.y < y0 - TOL:
            r.report.left[net] = (f"{where}: its lane is {abs(p - x):.2f} mm aside, too far to "
                                  f"reach by y {y0:.2f}")
            continue
        pen.vert(y0).diag(_mm(dp.GetX()))
        if pen.y < r.box(dp, layer)[3] - TOL:
            r.report.left[net] = f"{where}: the jog to its pad does not end below the pad"
            continue
        pen.vert(_mm(dp.GetY()))
        r.offer(net, name, pen.items)


def lriver(r: "Router", name: str, members: list, *, layer: int, row_top: float,
           approach: tuple | None = None, gap: float = LANE_GAP, n_rows: int = 24) -> dict:
    """Pens for a river that goes up from a row of starts (fingers) to a row of ends higher up,
    turning onto horizontal rows between them: row k at row_top + PITCH k (k 0 nearest the
    ends). Of two nets moving the same way whose rows overlap, the outer one (the farther from
    the river's middle) takes the row nearer the ends, so no two cross. `approach` (y_end,
    y_lo): lanes in that band clear obstacles before the ends, then a 45-degree step onto the
    end's x. members: (net, start point, end point). Returns net -> Pen (ended on the end
    point); a net with no free row is reported and left out."""
    members = sorted(members, key=lambda m: (m[1][0], m[0]))
    nets = [n for n, _, _ in members]
    sx = [s[0] for _, s, _ in members]
    ex = [e[0] for _, _, e in members]
    if approach:
        y_end, y_lo = approach
        band = (min(ex) - 2, y_end + 0.05, max(ex) + 2, y_lo)
        def shape(i, p):
            e = members[i][2]
            pen = Pen((p, y_lo), layer, r.space.width(nets[i]))
            return pen.vert(y_end + abs(p - e[0])).diag(e[0]).vert(e[1]).items
        px = choose_lanes(r, ex, band, layer, nets, gap, reach=y_lo - y_end, shape=shape)
    else:
        px = list(ex)
    need = _need(r, nets)

    def blocked_at(k, a, b):
        y = row_top + PITCH * k
        return bool(blocked(r.space, layer, (a, y - need, b, y + need), need,
                            skip_nets=set(nets), soft=True))

    spans = []
    for net, x, p in zip(nets, sx, px):
        if p is None or abs(p - x) < TOL:
            continue
        spans.append((net, x, p, x if p > x else -x))
    row_of = rows(spans, blocked_at, n_rows)
    pens = {}
    for (net, s, e), x, p in zip(members, sx, px):
        if p is None:
            r.report.left[net] = f"{name}: {net}: no lane past the obstacles before its end"
            continue
        moving = abs(p - x) >= TOL
        if moving and net not in row_of:
            r.report.left[net] = (f"{name}: {net}: no free row between x {min(x, p):.2f} and "
                                  f"{max(x, p):.2f}")
            continue
        pen = Pen(s, layer, r.space.width(net))
        if moving:
            pen.vert(row_top + PITCH * row_of[net]).horiz(p)
        if approach and abs(p - e[0]) > TOL:
            pen.vert(approach[0] + abs(p - e[0])).diag(e[0])
        pen.vert(e[1])
        pens[net] = pen
    return pens


# --- the carrier's rivers ---------------------------------------------------------------------

def _courtyard(r: "Router", ref: str) -> tuple:
    bb = r.part(ref).GetCourtyard(pcbnew.F_CrtYd).BBox()
    return (_mm(bb.GetLeft()), _mm(bb.GetTop()), _mm(bb.GetRight()), _mm(bb.GetBottom()))


def _pads_in(r: "Router", box: tuple, layer: int, skip=()) -> list[Obs]:
    out = []
    for i in r.space.near(box):
        o = r.space.obs[i]
        if o.pad and o.kind == "poly" and layer in o.layers and o.net not in skip:
            x0, y0, x1, y1 = o.box
            if not (x1 < box[0] or x0 > box[2] or y1 < box[1] or y0 > box[3]):
                out.append(o)
    return out


def _j2_front_bottom(r: "Router") -> float:
    j2 = r.part("J2")
    yc = _mm(j2.GetY())
    return max(r.box(p)[3] for p in j2.Pads() if p.GetNumber().isdigit() and _mm(p.GetY()) > yc)


def river_data(r: "Router", river: pb.River) -> None:
    """U1's FPGA side to J2's rear row from above (module doc): the channel, U4's side, a fan
    onto a via row under J2's front row, the chimney up J2's unused left end, the highway."""
    require(river, pb.REAR, pb.ABOVE)
    nets = list(river.nets)
    src = {n: r.pad(n, river.ref) for n in nets}
    dst = {n: r.pad(n, "J2") for n in nets}
    order = sorted(nets, key=lambda n: _mm(src[n].GetX()))
    k_max = len(order) - 1
    need = _need(r, nets)
    y_u1 = min(r.box(p)[1] for p in src.values())                 # U1's FPGA pins, top end
    u4 = r.part("U4")
    u4_boxes = [r.box(p) for p in u4.Pads()]
    y_u4_low = max(b[3] for b in u4_boxes)                         # U4's lower row, bottom end
    y_u4_top = min(b[1] for b in u4_boxes)
    y_front = _j2_front_bottom(r)
    rear_top = min(r.box(dst[n])[1] for n in nets)                 # the rear pads' top end
    # the channel between U1 and U4: one row per net, the leftmost pin lowest
    row = {n: y_u1 - need - PITCH * k for k, n in enumerate(order)}
    if row[order[-1]] < y_u4_low + need - TOL:
        raise CannotLay("the channel under U4 is too narrow for the data bus")
    # up U4's left side, outside its courtyard
    col8 = _courtyard(r, "U4")[0] - need
    col = {n: col8 - PITCH * (k_max - k) for k, n in enumerate(order)}
    # sideways past what stands over U4's left end (C14), before reaching it
    over = _pads_in(r, (col[order[0]] - 0.2, y_front + 0.1, col8 + need, y_u4_top - 0.01), F,
                    skip=nets)
    shift = max(0.0, col8 - (min(o.box[0] for o in over) - need)) if over else 0.0
    left = _pads_in(r, (col[order[0]] - shift - need, y_u4_top, col[order[0]], y_u1), F,
                    skip=nets)
    y_end0 = (min(o.box[1] for o in left) - need) if left else y_u4_top
    stair = PITCH * math.sqrt(2) - PITCH + 0.01                    # parallel 45s stay a pitch apart
    # a row of vias under the front row, and the fan onto it (the leftmost net highest)
    y_via = y_front + VIA_MM / 2 + rt.CLEARANCE_MM + MARGIN + 0.01
    vias_left = [o.geom[0] for o in r.space.obs if o is not None and o.kind == "cap"
                 and o.geom[0] == o.geom[1] and o.geom[0][0] < col8
                 and rear_top - 1 < o.geom[0][1] < y_via + VIA_MM]
    v0 = max([p[0] + VIA_TRACK for p in vias_left]
             + [p[0] + VIA_PITCH for p in vias_left if abs(p[1] - y_via) < VIA_MM]
             + [col[order[0]] - shift])
    v0 = math.ceil(v0 * 100) / 100
    # the highway: one row per net, 0.6 mm apart (a via between two rows keeps its clearance)
    h_last = rear_top - VIA_MM / 2 - rt.CLEARANCE_MM - MARGIN - 0.01
    for k, n in enumerate(order):
        pen = Pen(_pt(src[n].GetPosition()), F, r.space.width(n))
        # the first net's jog starts clear of what is on its left; each next one a stair higher
        pen.vert(row[n]).horiz(col[n]).vert(y_end0 - stair * k).diag(col[n] - shift)
        pen.vert(y_via + VIA_TRACK + PITCH * k).horiz(v0 + VIA_PITCH * k).vert(y_via).via(B)
        pen.vert(h_last - 0.6 * (k_max - k)).horiz(_mm(dst[n].GetX())).via(F)
        pen.vert(_mm(dst[n].GetY()))
        r.offer(n, river.name, pen.items)


def river_listen(r: "Router", river: pb.River) -> None:
    """U4's FPGA side to J2's rear row from below: stubs to staggered vias above U4, B.Cu at 45
    degrees under the front row, staggered vias in the gap, stubs up into the rear pads."""
    require(river, pb.REAR, pb.BELOW_B)
    nets = list(river.nets)
    src = {n: r.pad(n, river.ref) for n in nets}
    dst = {n: r.pad(n, "J2") for n in nets}
    order = sorted(nets, key=lambda n: _mm(src[n].GetX()))
    y_top = min(r.box(p)[1] for p in src.values())
    step = VIA_MM / 2 + rt.CLEARANCE_MM + MARGIN + 0.01
    near, far = y_top - step, y_top - step - 0.92
    y0 = far - VIA_TRACK
    y_gap_a = max(r.box(p)[3] for p in dst.values()) + step
    gap_rows = (y_gap_a, y_gap_a + 0.92)
    chosen: dict[str, float] = {}
    for k in range(len(order) - 1, -1, -1):             # right to left, alternating rows
        n = order[k]
        x = _mm(dst[n].GetX())
        right = chosen.get(order[k + 1]) if k + 1 < len(order) else None
        best = None
        for y in sorted(gap_rows, key=lambda y: (y == right, y)):
            if any(math.dist((x, y), (_mm(dst[m].GetX()), chosen[m])) < VIA_PITCH
                   for m in chosen):
                continue
            hard, soft = r.space.check(n, Via((x, y)))
            if not hard and (best is None or (best[0] and not soft)):
                best = (bool(soft), y)
        if best is not None:
            chosen[n] = best[1]
    for k, n in enumerate(order):
        if n not in chosen:
            r.report.left[n] = f"{river.name}: {n}: no room for its via in the gap"
            continue
        pen = Pen(_pt(src[n].GetPosition()), F, r.space.width(n))
        pen.vert(near if k % 2 == 0 else far).via(B).vert(y0).diag(_mm(dst[n].GetX()))
        pen.vert(chosen[n]).via(F).vert(_mm(dst[n].GetY()))
        r.offer(n, river.name, pen.items)


def river_address(r: "Router", river: pb.River) -> None:
    """U2's FPGA side to J2's front row on F.Cu: lanes round C8 and C9, then one jog. A net with
    two pins on the part (slot_a_oe_n) leaves from the leftmost, as plan_buses plans it."""
    require(river, pb.FRONT, pb.BELOW_F)
    nets = list(river.nets)
    src = {n: r.pad(n, river.ref, pick=lambda ps: min(ps, key=lambda p: p.GetX())) for n in nets}
    ye = min(r.box(p)[1] for p in src.values())
    xs = [_mm(p.GetX()) for p in src.values()]
    above = _pads_in(r, (min(xs) - 1, ye - 4, max(xs) + 1, ye - 0.05), F, skip=nets)
    y0 = min(o.box[1] for o in above) - _need(r, nets) if above else ye - 1.0
    vriver(r, river.name, [(n, src[n], r.pad(n, "J2")) for n in nets], y0=y0)


def river_control(r: "Router", river: pb.River) -> None:
    """U5's FPGA side straight up into the front pads above it."""
    require(river, pb.FRONT, pb.BELOW_F)
    nets = list(river.nets)
    src = {n: r.pad(n, river.ref) for n in nets}
    ye = min(r.box(p)[1] for p in src.values())
    vriver(r, river.name, [(n, src[n], r.pad(n, "J2")) for n in nets], y0=ye - 1.5)


def _finger(r: "Router", net: str) -> P:
    """The point a track leaves a finger from: 0.4 mm inside its top end."""
    p = r.pad(net, "J1")
    return (_mm(p.GetX()), r.box(p)[1] + 0.4)


def river_slot_address(r: "Router", river: pb.River) -> None:
    """J1 A0-A15 (front fingers) to U2's slot pins on F.Cu: nested rows under the part row,
    lanes round C10 and C11, up into the pins."""
    require(river, pb.FINGERS, pb.FRONT_SIDE)
    nets = list(river.nets)
    ends = {n: r.pad(n, "U2") for n in nets}
    y_end = max(r.box(p)[3] for p in ends.values())                # U2's slot pins, bottom end
    starts = {n: _finger(r, n) for n in nets}
    xs = [s[0] for s in starts.values()]
    exs = [_mm(p.GetX()) for p in ends.values()]
    need = _need(r, nets)
    y_lo = max(o.box[3] for o in _pads_in(r, (min(exs) - 1, y_end + 0.05, max(exs) + 1,
                                              y_end + 4), F, skip=nets)) + need
    lane = _pads_in(r, (min(xs), y_end + 0.05, max(xs), min(s[1] for s in starts.values()) - 3),
                    F, skip=nets)
    row_top = max([y_lo] + [o.box[3] + need for o in lane])
    members = [(n, starts[n], _pt(ends[n].GetPosition())) for n in nets]
    pens = lriver(r, river.name, members, layer=F, row_top=row_top, approach=(y_end, y_lo))
    for n in nets:
        if n in pens:
            r.offer(n, river.name, pens[n].items)


def river_slot_data(r: "Router", river: pb.River) -> None:
    """J1 D0-D7 (back fingers) on B.Cu: nested rows below U1, a via under U1's body with a stub
    to its pin, on at 45 degrees under the channel, a via under U4's body with a stub to its
    pin."""
    require(river, pb.FINGERS, pb.BACK)
    nets = list(river.nets)
    u1 = {n: r.pad(n, "U1") for n in nets}
    u4 = {n: r.pad(n, "U4") for n in nets}
    order = sorted(nets, key=lambda n: _mm(u1[n].GetX()))
    step = VIA_MM / 2 + rt.CLEARANCE_MM + MARGIN + 0.01
    a = min(r.box(p)[1] for p in u1.values()) - step              # under U1's body
    c = min(r.box(p)[1] for p in u4.values()) - step              # under U4's body
    via1 = {n: a - 0.92 * (k % 2) for k, n in enumerate(order)}
    via4 = {n: c - 0.92 * (k % 2) for k, n in enumerate(order)}
    y_mid = min(_mm(p.GetY()) for p in r.part("U1").Pads())
    members = [(n, _finger(r, n), (_mm(u1[n].GetX()), via1[n])) for n in nets]
    pens = lriver(r, river.name, members, layer=B, row_top=a + VIA_TRACK)
    for n in order:
        if n not in pens:
            continue
        pen = pens[n].via(B)
        stub1 = Seg(F, (pen.x, pen.y), _pt(u1[n].GetPosition()), r.space.width(n))
        pen.vert(y_mid).diag(_mm(u4[n].GetX())).vert(via4[n]).via(B)
        stub4 = Seg(F, (pen.x, pen.y), _pt(u4[n].GetPosition()), r.space.width(n))
        r.offer(n, river.name, pen.items + [stub1, stub4])


# --- the power paths (P29) ------------------------------------------------------------------
#
# The reviewer's probe joined every net left after the rivers with the A* except +5V and
# +5V_MOD: at the Power class's 0.5 mm the A* cannot enter U2's 0.5 mm-pitch VCCB pins, and
# left to itself it runs +5V_MOD across the middle of the board, through the band every other
# net must cross. So they are planned: +5V_MOD round the edges and through the highway, +5V
# into U2 by vias under its body. Every piece is offered on its own, so one that does not fit
# is reported and the rest kept. What the pieces leave apart (+5V's groups) is Freerouting's to
# join, with all the other nets it is handed at once: it has no order of ours. They are not
# joined here: the A* joining them before the single nets took the corridor from U2's corner to
# U3 that slot_a_dir needs (checked on the carrier). What Freerouting leaves, route.py's finisher
# joins, in net-name order (which puts +5V ahead of the signals).

NECK_MM = 0.3                      # a Power-class neck: hw-drc allows 0.3 mm for at most 1.5 mm
NECK_MAX_MM = 1.45


def _overlap(a: tuple, b: tuple) -> tuple | None:
    lo, hi = max(a[0], b[0]), min(a[1], b[1])
    return (lo, hi) if hi > lo else None


def _inset(p: P, box: tuple, d: float) -> P:
    return (min(max(p[0], box[0] + d), box[2] - d), min(max(p[1], box[1] + d), box[3] - d))


def link_items(r: "Router", p, q, w: float) -> list | None:
    """Copper joining two pads of one net on their layer: a bar (width w) straight across where
    their x or y ranges overlap by w, else a neck corner to corner (NECK_MM, at most
    NECK_MAX_MM long); None if a neck would be longer."""
    layer = F if p.IsOnLayer(F) else B
    bp, bq = r.box(p, layer), r.box(q, layer)
    cp, cq = _pt(p.GetPosition()), _pt(q.GetPosition())
    ox, oy = _overlap((bp[0], bp[2]), (bq[0], bq[2])), _overlap((bp[1], bp[3]), (bq[1], bq[3]))
    if ox and ox[1] - ox[0] >= w:
        x = (ox[0] + ox[1]) / 2
        return [Seg(layer, (x, cp[1]), (x, cq[1]), w)]
    if oy and oy[1] - oy[0] >= w:
        y = (oy[0] + oy[1]) / 2
        return [Seg(layer, (cp[0], y), (cq[0], y), w)]
    d = NECK_MM / 2 + 0.01
    a = _inset(cq, bp, d)
    b = _inset(a, bq, d)
    if math.dist(a, b) > NECK_MAX_MM:
        return None
    return [Seg(layer, a, b, NECK_MM)]


def loop_items(r: "Router", p, q, w: float) -> list[list]:
    """Copper joining two pads one above the other round what sits between them: out to the
    side by w, along, and back, on the right of both pads and on the left (in that order). The
    side runs midway between the pads' column and the nearest pad beyond it."""
    layer = F if p.IsOnLayer(F) else B
    cp, cq = _pt(p.GetPosition()), _pt(q.GetPosition())
    bp, bq = r.box(p, layer), r.box(q, layer)
    out = []
    for side in (1, -1):
        edge = max(bp[2], bq[2]) if side > 0 else min(bp[0], bq[0])
        lo, hi = min(cp[1], cq[1]), max(cp[1], cq[1])
        beyond = [o.box[0] if side > 0 else o.box[2] for o in
                  _pads_in(r, (edge, lo, edge + 3, hi) if side > 0 else (edge - 3, lo, edge, hi),
                           layer, skip={p.GetNetname()})
                  if (o.box[0] > edge if side > 0 else o.box[2] < edge)]
        far = (min(beyond) if side > 0 else max(beyond)) if beyond else edge + side * 2.0
        x = (edge + far) / 2
        pen = Pen(cp, layer, w).horiz(x).vert(cq[1]).horiz(cq[0])
        out.append(pen.items)
    return out


def _label(p) -> str:
    return f"{p.GetParentFootprint().GetReference()}.{p.GetNumber()}"


def join_pads(r: "Router", net: str, pads: list, w: float) -> None:
    """Join a small group of a net's pads (one piece per link), nearest first (Prim, from the
    first pad): a bar or a neck, else a loop round what is between; a link that fits none is
    reported."""
    done, rest = [pads[0]], list(pads[1:])
    while rest:
        p, q = min(((p, q) for p in done for q in rest),
                   key=lambda pq: (math.dist(_pt(pq[0].GetPosition()), _pt(pq[1].GetPosition())),
                                   _label(pq[0]), _label(pq[1])))
        rest.remove(q)
        done.append(q)
        piece = f"{_label(p)}-{_label(q)}"
        tries = [t for t in [link_items(r, p, q, w)] if t] + loop_items(r, p, q, w)
        for items in tries:
            hard = [h for it in items for h in r.space.check(net, it)[0]]
            if not hard:
                r.offer(net, "power", items, piece)
                break
        else:
            first = next((h for it in tries[0] for h in r.space.check(net, it)[0]), "")
            r.report.left[f"{net} {piece}"] = f"power: no bar, neck or loop fits: {first}"


def _groups(pads: list, near: float = 3.0) -> list[list]:
    """The pads in groups of pads within `near` mm of one another (single linkage), each sorted
    by position, the groups by their first pad."""
    groups: list[list] = []
    for p in sorted(pads, key=lambda p: (p.GetX(), p.GetY())):
        mine = [g for g in groups if any(math.dist(_pt(p.GetPosition()), _pt(q.GetPosition()))
                                         <= near for q in g)]
        merged = [p] + [q for g in mine for q in g]
        groups = [g for g in groups if g not in mine] + [sorted(merged, key=lambda q: (q.GetX(), q.GetY()))]
    return sorted(groups, key=lambda g: (g[0].GetX(), g[0].GetY()))


def power_5v_mod(r: "Router", river: pb.River) -> None:
    """+5V_MOD: from D1 up the right edge (outside J2's peg and boss), across the highway under
    the module (between the top headers and the pegs), down the left edge into the regulator's
    cluster (VR1, C24-C26); a bar with necks into J2's 5V pins; the cluster joined pad to pad."""
    net = river.nets[0]
    w = r.space.width(net)
    need = r.space.clearance(net) + w / 2 + MARGIN
    d1 = r.pad(net, river.ref)
    j2 = r.part("J2")
    pins = sorted((p for p in j2.Pads() if p.GetNetname() == net), key=lambda p: p.GetX())
    if not pins:
        raise CannotLay(f"J2 has no pad on {net}")
    cluster = sorted((p for fp in r.board.GetFootprints() if fp.GetReference() not in (river.ref, "J2")
                      for p in fp.Pads() if p.GetNetname() == net),
                     key=lambda p: (p.GetY(), p.GetX()))
    if not cluster:
        raise CannotLay(f"{net} has no regulator side")
    top = cluster[0]                                   # the cluster's pad nearest J2
    def area(p):
        x0, y0, x1, y1 = r.box(p)
        return (x1 - x0) * (y1 - y0)
    pegs = [p for p in j2.Pads() if p.GetAttribute() == pcbnew.PAD_ATTRIB_SMD
            and not p.GetNetCode() and area(p) > 4.0]      # the solder pegs (MP), not the pins
    if not pegs:
        raise CannotLay("J2 has no solder pegs to route between")
    holes = [p for p in j2.Pads() if p.HasHole()]
    right = max([r.box(p)[2] + need for p in pegs]
                + [_mm(p.GetX()) + _mm(p.GetDrillSize().x) / 2 + r.space.hole_cl + w / 2 + MARGIN
                   for p in holes])
    right = math.ceil(right * 100) / 100
    peg_top = min(r.box(p)[1] for p in pegs)
    above = max(o.box[3] for o in r.space.obs if o is not None and o.pad and o.kind == "poly"
                and o.box[3] < peg_top - 1.0)
    y_h = round((above + peg_top) / 2, 2)
    rear_top = min(r.box(p)[1] for p in pins)
    y_bar = round(rear_top + 0.4 - NECK_MAX_MM + 0.05, 2)      # each neck 1.4 mm
    c = _pt(d1.GetPosition())
    trunk = Pen(c, F, w).vert(r.box(d1)[1] - 0.6).diag(right).vert(y_h)
    trunk.horiz(_mm(top.GetX())).vert(_mm(top.GetY()))
    r.offer(net, "power", trunk.items, f"trunk {_label(d1)}-{_label(top)}")
    bar = [Seg(F, (_mm(top.GetX()), y_bar), (_mm(pins[-1].GetX()), y_bar), w)]
    bar += [Seg(F, (_mm(p.GetX()), y_bar), (_mm(p.GetX()), rear_top + 0.4), NECK_MM) for p in pins]
    r.offer(net, "power", bar, f"J2 pins {pins[0].GetNumber()}-{pins[-1].GetNumber()}")
    join_pads(r, net, cluster, w)


def power_5v(r: "Router", river: pb.River) -> None:
    """+5V: each group of its pads joined in place (D1-C2-F1, U3 and its capacitor, each of
    U2's VCCB pins and its capacitor); U2's pins (0.5 mm pitch, too fine for 0.5 mm copper) by
    a neck to a via under U2's body each, the vias joined on B.Cu across the body's middle. The
    groups are Freerouting's to join (see above)."""
    net = river.nets[0]
    w = r.space.width(net)
    pads = [p for fp in r.board.GetFootprints() for p in fp.Pads() if p.GetNetname() == net]
    if not pads:
        raise CannotLay(f"no pad on {net}")
    for g in _groups(pads):
        if len(g) > 1:
            join_pads(r, net, g, w)
    u2 = r.part(river.ref)
    fine = sorted((p for p in u2.Pads() if p.GetNetname() == net), key=lambda p: p.GetX())
    pin_rows = sorted({round(_mm(p.GetY()), 3) for p in u2.Pads()})
    y_mid = (pin_rows[0] + pin_rows[-1]) / 2
    step = VIA_MM / 2 + rt.CLEARANCE_MM + MARGIN + 0.01
    vias = []
    for p in fine:
        bx = r.box(p)
        inner_up = _mm(p.GetY()) > y_mid                # the pin's inner end faces the middle
        y_in = bx[1] if inner_up else bx[3]
        at = (_mm(p.GetX()), y_in - step if inner_up else y_in + step)
        neck = Seg(F, (_mm(p.GetX()), y_in + (0.3 if inner_up else -0.3)), at, NECK_MM)
        if r.offer(net, "power", [neck, Via(at)], f"{_label(p)} via"):
            vias.append(at)
    if len(vias) > 1:
        pen = Pen(vias[0], B, w).vert(y_mid)
        for at in vias[1:]:
            pen.horiz(at[0])
        pen.vert(vias[-1][1])
        r.offer(net, "power", pen.items, f"{river.ref} B link")


_ADDRESS = next(r.ref for r in pb.RIVERS if r.name == "address")      # U2
POWER = (pb.River("+5V_MOD", "D1", ("+5V_MOD",), "power", "edges and highway"),
         pb.River("+5V", _ADDRESS, ("+5V",), "power", f"vias under {_ADDRESS}'s body"))
POWER_LAYOUTS = {"+5V_MOD": power_5v_mod, "+5V": power_5v}


# The layouts, by plan_buses' river names. A river the plan has and no layout here is not
# attempted: its nets are reported as left, with why.
LAYOUTS = {"data": river_data, "listen": river_listen, "address": river_address,
           "control": river_control, "slot_data": river_slot_data,
           "slot_address": river_slot_address}
CLOCKS = "clocks"                  # rivers laid by the clock step (the Clock class, A*)
NOT_ATTEMPTED = {
    "data_oe": "the first pass lays it: one pin on U1's slot-side row, beside C7, with its "
               "pull-up R12.",
    "host": "Freerouting lays them: the header lines start at the top edge, a short run down to "
            "the rear row.",
    "rest": "Freerouting lays them: single nets, each to the nearest J2 pad the rivers left, in "
            "either row.",
}


def carrier_plan() -> list[tuple]:
    """plan_buses' rivers (J2's, then the slot side's), each with its layout, CLOCKS, or the
    reason it is not attempted; then the power paths (POWER, P29)."""
    out = []
    for river in pb.RIVERS + pb.SLOT_RIVERS:
        if river.name in LAYOUTS:
            out.append((river, LAYOUTS[river.name]))
        elif river.name.startswith("clocks"):
            out.append((river, CLOCKS))
        else:
            out.append((river, NOT_ATTEMPTED.get(
                river.name, "Freerouting lays them: bus_route has no layout for this river.")))
    return out + [(river, POWER_LAYOUTS[river.name]) for river in POWER]


def run(board, plan=None, clocks: bool = True) -> Report:
    """Lay the plan's rivers and power pieces on `board` (after route.start: the P19 fanout),
    move the plane vias in their way, lock everything, then the clocks (A*, Clock class). A power
    net the pieces leave apart is listed as left. Every planned net ends up
    laid or in the report's `left` with its reason; a river the board cannot take (CannotLay)
    is skipped there, and its nets not laid by then are listed (a net laid in pieces as
    'NET rest')."""
    plan = carrier_plan() if plan is None else plan
    r = Router(board)
    clock_rivers = []
    for river, layout in plan:
        if callable(layout):
            try:
                layout(r, river)
            except CannotLay as e:
                laid = {k for keys in r.report.laid.values() for k in keys}
                for n in river.nets:
                    if n in laid:                    # laid whole before the layout stopped
                        continue
                    if any(k.startswith(f"{n} ") for k in laid):
                        r.report.left.setdefault(f"{n} rest", f"{river.name}: the rest of the "
                                                 f"layout skipped: {e}")
                    else:
                        r.report.left.setdefault(n, f"{river.name}: river skipped: {e}")
        elif layout == CLOCKS:
            clock_rivers.append(river)
        else:
            for n in river.nets:
                r.report.left[n] = f"{river.name}: not attempted: {layout}"
    r.relocate()
    r.lay()
    if clocks:
        names = clock_order(board)
        r.report.clocks_left = rt.lay_first(board, names)
        laid = [n for n in names if n not in r.report.clocks_left]
        if laid:
            r.report.laid.setdefault("clocks", []).extend(laid)
    for river in clock_rivers:
        for n in river.nets:
            if not clocks:
                r.report.left[n] = f"{river.name}: the clock step was not run"
            elif n in r.report.clocks_left:
                r.report.left[n] = f"{river.name}: the clock step found no path"
    for river, layout in plan:                # P29: what the planned pieces leave apart
        if river.row == "power" and callable(layout):
            for n in river.nets:
                k = len(rt._components(board, board.FindNet(n).GetNetCode()))
                if k > 1:
                    r.report.left[f"{n} joins"] = (
                        f"power: {k} pieces for Freerouting to join at the Power width, with "
                        "the other nets it routes (not here: the A* joining them before the "
                        "single nets walls in slot_a_dir)")
    return r.report


def clock_order(board) -> list[str]:
    """The Clock-class nets, in the first pass's order (route.CLOCK_NETS), any others after."""
    cls = set(rt.clock_nets(board))
    return [n for n in rt.CLOCK_NETS if n in cls] + sorted(cls - set(rt.CLOCK_NETS))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("board", type=Path)
    ap.add_argument("--out", type=Path, help="write here (default: the board, in place)")
    ap.add_argument("--netlist", type=Path, help="carrier.net: print the slot pins' load")
    ap.add_argument("--no-clocks", action="store_true", help="leave the clocks out")
    a = ap.parse_args(argv)
    b = pcbnew.LoadBoard(str(a.board))
    rt.start(b)
    report = run(b, clocks=not a.no_clocks)
    for line in report.lines():
        print(line)
    out = a.out or a.board
    rt.save_board(b, out)
    print(f"wrote {out}")
    if a.netlist:
        from hw.tools import check_load, netlist
        rows_ = check_load.table(pcbnew.LoadBoard(str(out)), netlist.load(a.netlist),
                                 check_load.load_caps())
        laid = {n for nets in report.laid.values() for n in nets}
        for row in sorted(rows_, key=lambda x: -x["total"]):
            if row["net"] in laid:
                print(f"load {row['net']}: {row['total']:.1f} pF ({row['length']:.1f} mm, "
                      f"{row['vias']} vias, pins {row['pins']:.1f} pF)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
