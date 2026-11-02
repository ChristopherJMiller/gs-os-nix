# SPDX-License-Identifier: GPL-3.0-or-later
"""hw-congestion: a routability gate before routing. Can this placement route, and where is it
congested? A coarse, PathFinder-style global router, in seconds.

    python3 hw/tools/check_congestion.py BOARD.kicad_pcb [--heatmap PNG] [--rounds N]
    python3 hw/tools/check_congestion.py --selftest             # HW CONGESTION SELFTEST OK

The board is read as route.py would see it just before the router runs: every track and via is
dropped, and for a board with inner planes the GND/+3V3 fan-out vias (route.fan_out_planes, run
on the in-memory board, never saved) are added and block cells. Then:

* The grid is 0.5 mm. Signal layers are F.Cu and B.Cu. A track moves between neighbouring cells;
  the *edge* between two cells has a capacity per layer, in micrometres of track pitch. It is
  measured on a 0.05 mm raster of the layer: the free centre-line length across the edge. Pads
  (inflated by clearance + half a track), vias, keep-outs, the edge clearance and (on F.Cu / B.Cu
  of the part's side) the courtyard of an IC (U*) take it away. Free space across 0.5 mm carries
  0.5 / 0.30 tracks; a run bounded by obstacles gets half a track pitch more at each bounded end
  (a gap between two obstacles holds floor(len / pitch) + 1 tracks, not len / pitch).
  Capacity is therefore an upper bound: overflow on it is a real shortage.
* A pad may leave through the edges its own (inflated) copper touches, free of charge and of
  courtyard or clearance blockage: that is how a pin escapes. Passive courtyards do not block.
  The free zone around the pads of an IC is a 0.8 mm reach (a stub and a via).
* Nets on the planes (GND, +3V3 on 4 layers) are not routed: their pads join the plane by their
  fan-out vias, which are obstacles. On a 2-layer board GND (and any net with an outer pour) is
  excluded in the same way (the pour joins it) and is not demand: a pour fills what the signals
  leave, so this is optimistic by the little the pour's own bridging tracks take.
* Every other net is routed as the tree of its minimum spanning tree: each pin in Prim order is
  maze-routed (A*) from the tree so far. A net's demand on an edge is its pitch (track + clearance:
  0.30 mm signal; the Power class 0.65; the Clock class 0.50), and a via adds 0.10 to each edge of
  its cell on both layers and uses the cell's single via slot.
* Negotiated congestion: rounds of rip-up and re-route of the nets that use an overflowing edge,
  with a present-congestion factor that grows and a history cost that accrues on edges that
  stayed overflowing.

Pass means overflow 0 within the rounds and no plane via without room (route.fan_out_planes
'no room' pads are reported and fail the gate, as they stop route.py). An edge counts as over
only above TOL_UM = 100 um, per edge: across a wide cut of n edges that can hide up to about
0.33 track on each, so the gate is lenient there by design. FAIL names its reasons: nets with NO OPEN PATH (every
route between two of their pins crosses an edge of capacity 0), and OVERFLOW, with the worst edges
(coordinates, layer, demand over capacity in tracks) and the nets through them. A heat-map PNG
shows utilisation per layer. The result is deterministic.

A gate before routing must never fail a board that routes, so every model choice errs toward more
capacity; the calibration (hw/tests/test_check_congestion.py) holds plan 1's routed board to it.
"""
import argparse
import heapq
import math
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import pcbnew
from PIL import Image, ImageDraw

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from hw.tools import build_board as bb
from hw.tools import route

RES = 0.05                         # mm per raster sample
SUB = 10                           # samples per cell
CELL = RES * SUB                   # 0.5 mm
RES_UM = 50
PITCH_UM = 300                     # a signal track and its clearance
BONUS_UM = 125                     # (PITCH - RES) / 2: a bounded end of a free run
TRACK_MM = 0.15
CLEARANCE_MM = 0.15
TRACK_HALO = CLEARANCE_MM + TRACK_MM / 2        # pad edge to a track's centre line
VIA_R = bb.VIA_MM / 2
VIA_HALO = CLEARANCE_MM + VIA_R                 # pad edge to a via's centre
VIA_EXTRA_UM = 100                 # a via demand on each edge of its cell, both layers
TOL_UM = 100                       # one raster sample at each end of a run: the capacity's own error
VIA_COST = 6.0                     # in cells
ESCAPE_MM = 0.8                    # around an IC pad: the stub and via of its escape
EDGE_MM = bb.EDGE_CLEARANCE_MM
HOLE_CLEARANCE_MM = 0.25
ROUNDS = 40
PRES0 = 0.6
PRES_GROW = 1.4
HIST_GAIN = 0.5
MARGIN = 14                        # cells; the search box around a connection, then the board
LAYERS = (pcbnew.F_Cu, pcbnew.B_Cu)
LAYER_NAMES = ("F.Cu", "B.Cu")
EPS = 1e-9


def run_capacity_um(samples: int, open_lo: bool, open_hi: bool) -> int:
    """Track capacity (um of pitch) of one free run of `samples` raster samples across an edge.
    An end that stops at an obstacle gets BONUS_UM; an end that reaches the cell's boundary
    carries on into the next edge and gets none."""
    if samples <= 0:
        return 0
    return samples * RES_UM + (0 if open_lo else BONUS_UM) + (0 if open_hi else BONUS_UM)


def _edge_caps(free: np.ndarray) -> np.ndarray:
    """run_capacity_um summed over the free runs along the last axis of `free` (booleans)."""
    s = free.shape[-1]
    cap = np.zeros(free.shape[:-1], dtype=np.int64)
    cur = np.zeros(free.shape[:-1], dtype=np.int64)
    lo_open = np.zeros(free.shape[:-1], dtype=bool)
    for k in range(s):
        f = free[..., k]
        ended = (~f) & (cur > 0)
        cap += np.where(ended, cur * RES_UM + np.where(lo_open, 0, BONUS_UM) + BONUS_UM, 0)
        cur = np.where(f, cur + 1, 0)
        lo_open = np.where(f & (cur == 1), k == 0, lo_open)
    cap += np.where(cur > 0, cur * RES_UM + np.where(lo_open, 0, BONUS_UM), 0)
    return cap


# --- rasterising the board ----------------------------------------------------------------

def _rings(poly) -> list[list[tuple[float, float]]]:
    out = []
    for k in range(poly.OutlineCount()):
        o = poly.COutline(k)
        out.append([(pcbnew.ToMM(o.CPoint(i).x), pcbnew.ToMM(o.CPoint(i).y))
                    for i in range(o.PointCount())])
    return out


class Canvas:
    """One raster layer: a PIL image in mm space (origin x0, y0)."""

    def __init__(self, x0: float, y0: float, w_px: int, h_px: int):
        self.x0, self.y0 = x0, y0
        self.img = Image.new("L", (w_px, h_px), 0)
        self.d = ImageDraw.Draw(self.img)

    def px(self, x: float, y: float) -> tuple[float, float]:
        return ((x - self.x0) / RES, (y - self.y0) / RES)

    def poly(self, ring, grow_mm: float = 0.0, fill: int = 255) -> None:
        if len(ring) < 3:
            return
        pts = [self.px(*p) for p in ring]
        self.d.polygon(pts, fill=fill)
        if grow_mm > 0:
            self.d.line(pts + [pts[0]], fill=fill, width=max(1, round(2 * grow_mm / RES)),
                        joint="curve")

    def disc(self, x: float, y: float, r_mm: float, fill: int = 255) -> None:
        cx, cy = self.px(x, y)
        r = r_mm / RES
        self.d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=fill)

    def seg(self, a, b, r_mm: float) -> None:
        pa, pb = self.px(*a), self.px(*b)
        self.d.line([pa, pb], fill=255, width=max(1, round(2 * r_mm / RES)))
        self.disc(*a, r_mm)
        self.disc(*b, r_mm)

    def mask(self) -> np.ndarray:
        return np.asarray(self.img) > 0


def _pad_rings(pad, layer) -> list:
    return _rings(pad.GetEffectivePolygon(layer))


def _is_ic(fp) -> bool:
    return fp.GetReference().startswith("U")


def net_rules(board, name: str) -> tuple[float, float]:
    """(track width, clearance) in mm of a net's class. The classes live in the .kicad_pro; a
    board loaded without one still gets the carrier's Power and Clock classes by name."""
    nc = board.GetDesignSettings().m_NetSettings.GetEffectiveNetClass(name)
    w, c = pcbnew.ToMM(nc.GetTrackWidth()), pcbnew.ToMM(nc.GetClearance())
    if name in bb.POWER_NETS:
        w = max(w, bb.POWER_TRACK_MM)
    if name in bb.CLOCK_NETS:
        w, c = max(w, bb.CLOCK_TRACK_MM), max(c, bb.CLOCK_CLEARANCE_MM)
    return max(w, TRACK_MM), max(c, CLEARANCE_MM)


def pitch_um(board, name: str) -> int:
    """What a net takes of an edge: its width plus its clearance, or its half width plus its
    clearance plus a signal's half width beside a signal, whichever is more."""
    w, c = net_rules(board, name)
    mm = max(w + c, w / 2 + c + TRACK_MM / 2)
    return int(round(mm * 1000))


@dataclass
class Net:
    name: str
    pitch: int
    pins: list                                   # [(i, j, layers)] one per pad cell
    own: set = field(default_factory=set)        # edge ids its pads let it use for free
    charges: dict = field(default_factory=dict)  # edge id -> um it holds
    pathe: set = field(default_factory=set)      # edges its paths run along
    vias: set = field(default_factory=set)       # cells where it has a via
    tree: set = field(default_factory=set)       # nodes it has reached


class Model:
    """The board as the router sees it: edge capacities and the nets to route."""

    def __init__(self):
        self.x0 = self.y0 = 0.0          # mm, the grid's origin
        self.nx = self.ny = self.n = 0   # cells across, down, in all
        self.hn = self.el = 0            # H edges per layer, edges per layer
        self.cap: list[int] = []         # um of capacity per edge id (both layers)
        self.open_cap = 0
        self.via_ok: list[bool] = []     # a via fits somewhere in the cell
        self.blk: list = []              # per layer: raster of centre-line-blocked samples
        self.trk_raw: list = []
        self.court: list = []
        self.nets: list[Net] = []
        self.planes: set = set()
        self.pours: set = set()
        self.notes: list[str] = []


def excluded_nets(board) -> tuple[set, set]:
    """(the inner-plane nets, the nets with an outer pour): neither is routed."""
    planes = route.plane_nets(board)
    pours = {z.GetNetname() for z in board.Zones()
             if not z.GetIsRuleArea() and (z.IsOnLayer(pcbnew.F_Cu) or z.IsOnLayer(pcbnew.B_Cu))}
    return planes, pours - planes


def build_model(board) -> Model:
    """Everything the routing needs, from a board whose routing has been dropped (and whose
    plane fan-out, if any, has been added)."""
    m = Model()
    bb_ = board.GetBoardEdgesBoundingBox()
    x_lo, y_lo = pcbnew.ToMM(bb_.GetX()), pcbnew.ToMM(bb_.GetY())
    x_hi, y_hi = x_lo + pcbnew.ToMM(bb_.GetWidth()), y_lo + pcbnew.ToMM(bb_.GetHeight())
    m.x0 = math.floor((x_lo - 1.0) / CELL) * CELL
    m.y0 = math.floor((y_lo - 1.0) / CELL) * CELL
    m.nx = int(math.ceil((x_hi + 1.0 - m.x0) / CELL))
    m.ny = int(math.ceil((y_hi + 1.0 - m.y0) / CELL))
    w_px, h_px = m.nx * SUB, m.ny * SUB
    m.n = m.nx * m.ny
    planes, pours = excluded_nets(board)
    m.planes, m.pours = planes, pours
    skip = planes | pours

    trk = [Canvas(m.x0, m.y0, w_px, h_px) for _ in LAYERS]      # blocked for a track's centre
    via = Canvas(m.x0, m.y0, w_px, h_px)                         # blocked for a via's centre
    court = [Canvas(m.x0, m.y0, w_px, h_px) for _ in LAYERS]     # IC courtyards, minus escapes

    # the board edge: outside the outline, and within the edge clearance of it
    outline = pcbnew.SHAPE_POLY_SET()
    board.GetBoardPolygonOutlines(outline, False)
    inside = Canvas(m.x0, m.y0, w_px, h_px)
    for k, ring in enumerate(_rings(outline)):
        inside.poly(ring, fill=255 if k == 0 else 0)
    for ring in _rings(outline):
        for cv, r in [(trk[0], EDGE_MM + TRACK_MM / 2), (trk[1], EDGE_MM + TRACK_MM / 2),
                      (via, EDGE_MM + VIA_R)]:
            pts = [cv.px(*p) for p in ring]
            cv.d.line(pts + [pts[0]], fill=255, width=round(2 * r / RES), joint="curve")
    inside_m = inside.mask()
    outside = ~inside_m

    for fp in board.GetFootprints():
        ic = _is_ic(fp)
        side = 0 if fp.GetLayer() == pcbnew.F_Cu else 1
        if ic:
            cy = fp.GetCourtyard(fp.GetLayer())
            if cy.OutlineCount():
                tmp = Canvas(m.x0, m.y0, w_px, h_px)
                for k, ring in enumerate(_rings(cy)):
                    tmp.poly(ring, fill=255 if k == 0 else 0)
                for pad in fp.Pads():
                    for ring in _pad_rings(pad, fp.GetLayer()):
                        tmp.poly(ring, grow_mm=ESCAPE_MM, fill=0)
                court[side].d.bitmap((0, 0), tmp.img, fill=255)
        for pad in fp.Pads():
            if pad.GetAttribute() == pcbnew.PAD_ATTRIB_NPTH:
                p = pad.GetPosition()
                r = max(pcbnew.ToMM(pad.GetDrillSize().x), pcbnew.ToMM(pad.GetDrillSize().y)) / 2
                for cv in trk:
                    cv.disc(pcbnew.ToMM(p.x), pcbnew.ToMM(p.y), r + HOLE_CLEARANCE_MM + TRACK_MM / 2)
                via.disc(pcbnew.ToMM(p.x), pcbnew.ToMM(p.y), r + HOLE_CLEARANCE_MM + VIA_R)
                continue
            for li, layer in enumerate(LAYERS):
                if not pad.IsOnLayer(layer):
                    continue
                for ring in _pad_rings(pad, layer):
                    trk[li].poly(ring, grow_mm=TRACK_HALO)
                    via.poly(ring, grow_mm=VIA_HALO)
    for t in board.GetTracks():                      # the fan-out, once it has been added
        if t.GetClass() == "PCB_VIA":
            p = t.GetPosition()
            for cv in trk:
                cv.disc(pcbnew.ToMM(p.x), pcbnew.ToMM(p.y), pcbnew.ToMM(t.GetWidth(pcbnew.F_Cu)) / 2 + TRACK_HALO)
            via.disc(pcbnew.ToMM(p.x), pcbnew.ToMM(p.y), pcbnew.ToMM(t.GetWidth(pcbnew.F_Cu)) / 2 + VIA_HALO)
        else:
            li = 0 if t.GetLayer() == pcbnew.F_Cu else 1
            trk[li].seg((pcbnew.ToMM(t.GetStart().x), pcbnew.ToMM(t.GetStart().y)),
                        (pcbnew.ToMM(t.GetEnd().x), pcbnew.ToMM(t.GetEnd().y)),
                        pcbnew.ToMM(t.GetWidth()) / 2 + TRACK_HALO)
            via.seg((pcbnew.ToMM(t.GetStart().x), pcbnew.ToMM(t.GetStart().y)),
                    (pcbnew.ToMM(t.GetEnd().x), pcbnew.ToMM(t.GetEnd().y)),
                    pcbnew.ToMM(t.GetWidth()) / 2 + VIA_HALO)
    for z in board.Zones():                          # keep-outs
        if not z.GetIsRuleArea():
            continue
        rings = _rings(z.Outline())
        if z.GetDoNotAllowTracks():
            for li, layer in enumerate(LAYERS):
                if z.IsOnLayer(layer):
                    for k, ring in enumerate(rings):
                        trk[li].poly(ring, grow_mm=TRACK_MM / 2, fill=255 if k == 0 else 0)
        if z.GetDoNotAllowVias():
            for k, ring in enumerate(rings):
                via.poly(ring, grow_mm=VIA_R, fill=255 if k == 0 else 0)

    m.trk_raw = [outside | cv.mask() for cv in trk]
    m.court = [cv.mask() for cv in court]
    m.blk = [m.trk_raw[li] | m.court[li] for li in range(2)]
    vblk = outside | via.mask()

    # edge capacities per layer: H edge (i,j)-(i+1,j) first, then V edge (i,j)-(i,j+1)
    nx, ny = m.nx, m.ny
    m.hn = (nx - 1) * ny
    m.el = m.hn + nx * (ny - 1)
    cap = np.zeros(2 * m.el, dtype=np.int64)
    for li in range(2):
        blk = m.blk[li]
        cols = np.arange(1, nx) * SUB
        free_h = ~(blk[:, cols - 1] | blk[:, cols])                    # (h_px, nx-1)
        free_h = free_h.reshape(ny, SUB, nx - 1).transpose(0, 2, 1)     # (ny, nx-1, SUB)
        cap[li * m.el: li * m.el + m.hn] = _edge_caps(free_h).reshape(-1)
        rows = np.arange(1, ny) * SUB
        free_v = ~(blk[rows - 1, :] | blk[rows, :])                    # (ny-1, w_px)
        free_v = free_v.reshape(ny - 1, nx, SUB)
        cap[li * m.el + m.hn: (li + 1) * m.el] = _edge_caps(free_v).reshape(-1)
    m.cap = cap.tolist()
    m.open_cap = SUB * RES_UM                      # a free edge: 500 um
    vfree = (~vblk).reshape(ny, SUB, nx, SUB).any(axis=(1, 3))
    m.via_ok = vfree.reshape(-1).tolist()          # a via fits somewhere in the cell

    # pads: pins of the routed nets, and the edges each pad lets its own net use
    by_net: dict[str, dict] = {}
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            name = pad.GetNetname()
            if not name or pad.GetAttribute() == pcbnew.PAD_ATTRIB_NPTH or name in skip:
                continue
            p = pad.GetPosition()
            ci = int((pcbnew.ToMM(p.x) - m.x0) // CELL)
            cj = int((pcbnew.ToMM(p.y) - m.y0) // CELL)
            ls = tuple(li for li, layer in enumerate(LAYERS) if pad.IsOnLayer(layer))
            if not ls or not (0 <= ci < nx and 0 <= cj < ny):
                continue
            by_net.setdefault(name, {"pins": {}, "pads": []})
            by_net[name]["pins"].setdefault((ci, cj), set()).update(ls)
            by_net[name]["pads"].append((pad, ls))
    m.nets = []
    for name in sorted(by_net):
        pins = [(i, j, tuple(sorted(ls))) for (i, j), ls in sorted(by_net[name]["pins"].items())]
        if len(pins) < 2:
            continue
        net = Net(name, pitch_um(board, name), pins)
        for pad, ls in by_net[name]["pads"]:
            for li in ls:
                _own_edges(m, net, pad, li)
        m.nets.append(net)
    return m


def _own_edges(m: Model, net: Net, pad, li: int) -> None:
    """The edges a pad's inflated copper touches: its own net uses them without charge."""
    layer = LAYERS[li]
    for ring in _pad_rings(pad, layer):
        xs = [p[0] for p in ring]
        ys = [p[1] for p in ring]
        g = TRACK_HALO
        i0 = max(0, int((min(xs) - g - m.x0) // CELL))
        i1 = min(m.nx - 1, int((max(xs) + g - m.x0) // CELL))
        j0 = max(0, int((min(ys) - g - m.y0) // CELL))
        j1 = min(m.ny - 1, int((max(ys) + g - m.y0) // CELL))
        w, h = (i1 - i0 + 1) * SUB, (j1 - j0 + 1) * SUB
        cv = Canvas(m.x0 + i0 * CELL, m.y0 + j0 * CELL, w, h)
        cv.poly(ring, grow_mm=g)
        z = cv.mask()
        for j in range(j0, j1 + 1):
            for i in range(i0, i1 + 1):
                a = z[(j - j0) * SUB:(j - j0 + 1) * SUB, (i - i0) * SUB:(i - i0 + 1) * SUB]
                if not a.any():
                    continue
                # the cell holds pad copper: all four of its edges are the pad's own
                if i + 1 < m.nx:
                    net.own.add(li * m.el + j * (m.nx - 1) + i)
                if i > 0:
                    net.own.add(li * m.el + j * (m.nx - 1) + i - 1)
                if j + 1 < m.ny:
                    net.own.add(li * m.el + m.hn + j * m.nx + i)
                if j > 0:
                    net.own.add(li * m.el + m.hn + (j - 1) * m.nx + i)



# --- negotiated routing ------------------------------------------------------------------

@dataclass
class Result:
    model: Model
    overflow: float                  # tracks
    rounds: int
    seconds: float
    wirelength: float                # mm of routed tree
    over_edges: list                 # [(overflow tracks, edge id)] worst first
    blocked: list                    # [(net, "pinA", "pinB")]
    use: list
    nets_on: dict                    # edge id -> [net names]
    vias: int
    notes: list = field(default_factory=list)


class Router:
    def __init__(self, m: Model):
        self.m = m
        self.use = [0] * (2 * m.el)
        self.hist = [0.0] * (2 * m.el)
        self.vuse = [0] * m.n
        self.vhist = [0.0] * m.n
        self.pf = PRES0

    # node n = layer * N + j * nx + i
    def _incident(self, li: int, i: int, j: int) -> list[int]:
        m = self.m
        out = []
        if i + 1 < m.nx:
            out.append(li * m.el + j * (m.nx - 1) + i)
        if i > 0:
            out.append(li * m.el + j * (m.nx - 1) + i - 1)
        if j + 1 < m.ny:
            out.append(li * m.el + m.hn + j * m.nx + i)
        if j > 0:
            out.append(li * m.el + m.hn + (j - 1) * m.nx + i)
        return out

    def commit(self, net: Net, e: int, um: int) -> None:
        net.charges[e] = net.charges.get(e, 0) + um
        self.use[e] += um

    def uncommit(self, net: Net) -> None:
        for e, um in net.charges.items():
            self.use[e] -= um
        for c in net.vias:
            self.vuse[c] -= 1
        net.charges.clear()
        net.pathe.clear()
        net.vias.clear()
        net.tree.clear()

    def path(self, net: Net, sources: set, targets: set, box) -> list | None:
        m = self.m
        nx, ny, n_cells, el, hn = m.nx, m.ny, m.n, m.el, m.hn
        cap, use, hist = m.cap, self.use, self.hist
        own, pitch, pf = net.own, net.pitch, self.pf
        via_ok, vuse, vhist = m.via_ok, self.vuse, self.vhist
        i0, i1, j0, j1 = box
        tgt = next(iter(targets))
        ti = (tgt % n_cells) % nx
        tj = (tgt % n_cells) // nx
        best: dict[int, float] = {}
        prev: dict[int, int] = {}
        heap: list = []
        heappop, heappush = heapq.heappop, heapq.heappush
        for s in sorted(sources):
            r = s % n_cells
            si, sj = r % nx, r // nx
            best[s] = 0.0
            heappush(heap, (abs(si - ti) + abs(sj - tj), 0.0, s))
        inv = 1.0 / PITCH_UM
        while heap:
            f, g, n = heappop(heap)
            if g > best.get(n, 1e30):
                continue
            if n in targets:
                out = [n]
                while n in prev:
                    n = prev[n]
                    out.append(n)
                out.reverse()
                return out
            li = n // n_cells
            r = n - li * n_cells
            j = r // nx
            i = r - j * nx
            base = li * el
            nbrs = []
            if i + 1 <= i1:
                nbrs.append((n + 1, base + j * (nx - 1) + i))
            if i - 1 >= i0:
                nbrs.append((n - 1, base + j * (nx - 1) + i - 1))
            if j + 1 <= j1:
                nbrs.append((n + nx, base + hn + j * nx + i))
            if j - 1 >= j0:
                nbrs.append((n - nx, base + hn + (j - 1) * nx + i))
            for nn, e in nbrs:
                if e in own:
                    c = 1.0
                else:
                    ce = cap[e]
                    over = use[e] + pitch - (ce if ce == 0 or ce >= pitch else pitch)
                    c = (1.0 + hist[e]) * (1.0 + pf * over * inv) if over > 0 else 1.0 + hist[e]
                g2 = g + c
                if g2 < best.get(nn, 1e30):
                    best[nn] = g2
                    prev[nn] = n
                    rr = nn % n_cells
                    heappush(heap, (g2 + abs(rr % nx - ti) + abs(rr // nx - tj), g2, nn))
            if via_ok[r]:
                nn = n + n_cells if li == 0 else n - n_cells
                cong = 1.0 + (pf * 4 if vuse[r] >= 1 else 0.0)
                g2 = g + VIA_COST * (1.0 + vhist[r]) * cong
                if g2 < best.get(nn, 1e30):
                    best[nn] = g2
                    prev[nn] = n
                    heappush(heap, (g2 + abs(i - ti) + abs(j - tj), g2, nn))
        return None

    def route_net(self, net: Net) -> None:
        m = self.m
        nx, n_cells = m.nx, m.n
        pins = net.pins
        # Prim order over the pad cells
        order = [0]
        left = set(range(1, len(pins)))
        dist = {k: abs(pins[k][0] - pins[0][0]) + abs(pins[k][1] - pins[0][1]) for k in left}
        while left:
            k = min(left, key=lambda q: (dist[q], q))
            left.remove(k)
            order.append(k)
            for q in left:
                dist[q] = min(dist[q], abs(pins[q][0] - pins[k][0]) + abs(pins[q][1] - pins[k][1]))
        first = pins[order[0]]
        net.tree = {l * n_cells + first[1] * nx + first[0] for l in first[2]}
        for k in order[1:]:
            pi, pj, pls = pins[k]
            targets = {l * n_cells + pj * nx + pi for l in pls}
            if targets & net.tree:
                continue
            ti = [(n % n_cells) % nx for n in net.tree]
            tj = [(n % n_cells) // nx for n in net.tree]
            box = (max(0, min(min(ti), pi) - MARGIN), min(nx - 1, max(max(ti), pi) + MARGIN),
                   max(0, min(min(tj), pj) - MARGIN), min(m.ny - 1, max(max(tj), pj) + MARGIN))
            p = self.path(net, net.tree, targets, box)
            if p is None:
                p = self.path(net, net.tree, targets, (0, nx - 1, 0, m.ny - 1))
            if p is None:
                continue
            self._lay(net, p)

    def _lay(self, net: Net, p: list) -> None:
        m = self.m
        nx, n_cells = m.nx, m.n
        for a, b in zip(p, p[1:]):
            la, lb = a // n_cells, b // n_cells
            ra, rb = a % n_cells, b % n_cells
            if la != lb:
                if ra not in net.vias:
                    net.vias.add(ra)
                    self.vuse[ra] += 1
                    i, j = ra % nx, ra // nx
                    for li in (0, 1):
                        for e in self._incident(li, i, j):
                            if e not in net.own and m.cap[e] > 0:
                                self.commit(net, e, VIA_EXTRA_UM)
            else:
                lo, hi = min(ra, rb), max(ra, rb)
                i, j = lo % nx, lo // nx
                e = la * m.el + (j * (nx - 1) + i if hi - lo == 1 else m.hn + j * nx + i)
                if e not in net.own and e not in net.pathe:
                    net.pathe.add(e)
                    self.commit(net, e, net.pitch)
            net.tree.add(b)
        net.tree.add(p[0])

    # --- accounting ---
    def overflow_edges(self) -> list:
        """[(um over, edge)]. An edge with any capacity takes at least one net of any width
        (a wide track or a via alone always fits: the capacity is an average over cells)."""
        cap, use = self.m.cap, self.use
        widest: dict[int, int] = {}
        for net in self.m.nets:
            for e, um in net.charges.items():
                if um > widest.get(e, 0):
                    widest[e] = um
        out = []
        for e in range(len(cap)):
            if use[e] > cap[e]:
                c = cap[e] if cap[e] == 0 else max(cap[e], widest.get(e, 0))
                if use[e] > c + TOL_UM:
                    out.append((use[e] - c, e))
        return out

    def overflow(self) -> float:
        tracks = sum(o for o, _ in self.overflow_edges()) / PITCH_UM
        tracks += sum(max(0, v - 1) for v in self.vuse)
        return tracks


def open_components(m: Model) -> list[int]:
    """Label the nodes by the connected component of the graph of edges with capacity > 0, plus
    every edge some net owns, plus vias where one fits. A net whose pins are in two components
    cannot be routed at all."""
    nx, ny, n_cells = m.nx, m.ny, m.n
    owned = set()
    for net in m.nets:
        owned |= net.own
    parent = list(range(2 * n_cells))

    def find(a: int) -> int:
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    def union(a: int, b: int) -> None:
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[rb] = ra

    for li in range(2):
        base = li * n_cells
        for j in range(ny):
            for i in range(nx - 1):
                e = li * m.el + j * (nx - 1) + i
                if m.cap[e] > 0 or e in owned:
                    union(base + j * nx + i, base + j * nx + i + 1)
        for j in range(ny - 1):
            for i in range(nx):
                e = li * m.el + m.hn + j * nx + i
                if m.cap[e] > 0 or e in owned:
                    union(base + j * nx + i, base + (j + 1) * nx + i)
    for c in range(n_cells):
        if m.via_ok[c]:
            union(c, n_cells + c)
    return [find(a) for a in range(2 * n_cells)]


def _hpwl(net: Net) -> int:
    xs = [p[0] for p in net.pins]
    ys = [p[1] for p in net.pins]
    return (max(xs) - min(xs)) + (max(ys) - min(ys))


def negotiate(m: Model, rounds: int = ROUNDS) -> Result:
    t0 = time.time()
    r = Router(m)
    comp = open_components(m)
    blocked = []
    for net in m.nets:
        # a pin is any of its layers' nodes; tied by THT/both layers
        labels = []
        for (i, j, ls) in net.pins:
            labels.append({comp[l * m.n + j * m.nx + i] for l in ls})
        reach = labels[0]
        pending = list(range(1, len(labels)))
        grew = True
        while pending and grew:
            grew = False
            for k in list(pending):
                if labels[k] & reach:
                    reach |= labels[k]
                    pending.remove(k)
                    grew = True
        for k in pending:
            blocked.append((net.name, _pin_name(m, net.pins[0]), _pin_name(m, net.pins[k])))
    order = sorted(m.nets, key=lambda n: (_hpwl(n), n.name))
    for net in order:
        r.route_net(net)
    used = 0
    for rnd in range(rounds):
        over = r.overflow_edges()
        vover = {c for c, v in enumerate(r.vuse) if v > 1}
        if not over and not vover:
            break
        used = rnd + 1
        overset = {e for _, e in over}
        for o, e in over:
            r.hist[e] += HIST_GAIN * o / PITCH_UM
        for c in vover:
            r.vhist[c] += HIST_GAIN
        r.pf *= PRES_GROW
        todo = [n for n in order
                if any(k in overset for k in n.charges)
                or (n.vias & vover)]
        for net in todo:
            r.uncommit(net)
        for net in todo:
            r.route_net(net)
    over = sorted(r.overflow_edges(), reverse=True)
    nets_on: dict = {}
    for net in m.nets:
        for k in net.charges:
            nets_on.setdefault(k, []).append(net.name)
    wl = 0.0
    for net in m.nets:
        wl += len(net.pathe) * CELL
    return Result(m, r.overflow(), used, time.time() - t0, wl,
                  [(o / PITCH_UM, e) for o, e in over], blocked, list(r.use), nets_on,
                  sum(len(n.vias) for n in m.nets))


def _pin_name(m: Model, pin) -> str:
    i, j, _ = pin
    return f"({m.x0 + (i + .5) * CELL:.1f}, {m.y0 + (j + .5) * CELL:.1f})"


def edge_where(m: Model, e: int) -> tuple[int, str, float, float]:
    """(layer index, 'H'/'V', x mm, y mm of the edge's midpoint)."""
    li, k = divmod(e, m.el)
    if k < m.hn:
        j, i = divmod(k, m.nx - 1)
        return li, "H", m.x0 + (i + 1) * CELL, m.y0 + (j + .5) * CELL
    j, i = divmod(k - m.hn, m.nx)
    return li, "V", m.x0 + (i + .5) * CELL, m.y0 + (j + 1) * CELL


# --- entry points ------------------------------------------------------------------------

def prepare(board) -> list[str]:
    """Drop the routing and add the plane fan-out, in memory. Returns the pads with no room."""
    route.strip_routing(board)
    planes = route.plane_nets(board)
    if planes:
        _, no_room = route.fan_out_planes(board, tuple(sorted(planes)))
        return no_room
    return []


def analyze(board, rounds: int = ROUNDS) -> Result:
    no_room = prepare(board)
    m = build_model(board)
    res = negotiate(m, rounds)
    if no_room:
        res.notes.append("no room for a plane via at " + ", ".join(no_room))
    return res


def problems(res: Result, worst: int = 8) -> list[str]:
    out = []
    m = res.model
    for name, a, b in res.blocked:
        out.append(f"NO OPEN PATH: {name}, from {a} to {b}: every way crosses an edge with no "
                   "capacity")
    if res.overflow > EPS:
        out.append(f"OVERFLOW {res.overflow:.1f} tracks over {len(res.over_edges)} edges after "
                   f"{res.rounds} rounds")
        for o, e in res.over_edges[:worst]:
            li, d, x, y = edge_where(m, e)
            nets = res.nets_on.get(e, [])
            out.append(f"  worst: {LAYER_NAMES[li]} {d}-edge at ({x:.1f}, {y:.1f}) mm: demand "
                       f"{res.use[e] / PITCH_UM:.1f} over capacity {m.cap[e] / PITCH_UM:.1f} "
                       f"tracks; nets {', '.join(sorted(nets)[:8])}"
                       + (" ..." if len(nets) > 8 else ""))
    out += res.notes
    return out


def write_heatmap(res: Result, path: Path, scale: int = 5) -> None:
    """Utilisation per cell, one panel per layer: grey where nothing can be routed, white-to-
    orange with use, red where an edge overflows."""
    m = res.model
    hot = {e for _, e in res.over_edges}
    panels = []
    for li in range(2):
        util = np.zeros((m.ny, m.nx))
        over = np.zeros((m.ny, m.nx), dtype=bool)
        for e in range(li * m.el, (li + 1) * m.el):
            u = res.use[e]
            if u <= 0:
                continue
            k = e - li * m.el
            if k < m.hn:
                j, i = divmod(k, m.nx - 1)
                cells = [(j, i), (j, i + 1)]
            else:
                j, i = divmod(k - m.hn, m.nx)
                cells = [(j, i), (j + 1, i)]
            frac = u / max(m.cap[e], 1)
            for c in cells:
                util[c] = max(util[c], frac)
                if e in hot:
                    over[c] = True
        blk = m.blk[li].reshape(m.ny, SUB, m.nx, SUB).mean(axis=(1, 3))
        img = np.zeros((m.ny, m.nx, 3), dtype=np.uint8)
        img[:] = 255
        img[blk > 0.5] = (190, 190, 190)
        busy = util > 0
        shade = np.clip(util, 0, 1.0)
        for ch, base in enumerate((255, 200, 60)):
            img[..., ch] = np.where(busy, (255 - (255 - base) * shade).astype(np.uint8), img[..., ch])
        img[over] = (220, 0, 0)
        im = Image.fromarray(img).resize((m.nx * scale, m.ny * scale), Image.NEAREST)
        ImageDraw.Draw(im).text((3, 3), f"{LAYER_NAMES[li]}  overflow {res.overflow:.1f}", fill=(0, 0, 0))
        panels.append(im)
    out = Image.new("RGB", (panels[0].width * 2 + 8, panels[0].height), (255, 255, 255))
    out.paste(panels[0], (0, 0))
    out.paste(panels[1], (panels[0].width + 8, 0))
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    out.save(path)


# --- the selftest's synthetic board --------------------------------------------------------

def _new_board():
    b = pcbnew.BOARD()
    b.SetCopperLayerCount(2)
    nc = b.GetDesignSettings().m_NetSettings.GetDefaultNetclass()
    nc.SetTrackWidth(pcbnew.FromMM(TRACK_MM))      # the carrier's Default class
    nc.SetClearance(pcbnew.FromMM(CLEARANCE_MM))
    return b


def _rect_outline(b, x0, y0, x1, y1):
    pts = [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]
    for a, c in zip(pts, pts[1:] + pts[:1]):
        s = pcbnew.PCB_SHAPE(b)
        s.SetShape(pcbnew.SHAPE_T_SEGMENT)
        s.SetLayer(pcbnew.Edge_Cuts)
        s.SetStart(pcbnew.VECTOR2I(pcbnew.FromMM(a[0]), pcbnew.FromMM(a[1])))
        s.SetEnd(pcbnew.VECTOR2I(pcbnew.FromMM(c[0]), pcbnew.FromMM(c[1])))
        s.SetWidth(pcbnew.FromMM(0.1))
        b.Add(s)


def _keepout(b, x0, y0, x1, y1):
    z = pcbnew.ZONE(b)
    z.SetIsRuleArea(True)
    z.SetDoNotAllowTracks(True)
    z.SetDoNotAllowVias(True)
    z.SetDoNotAllowPads(False)
    z.SetDoNotAllowFootprints(False)
    z.SetLayerSet(pcbnew.LSET.AllCuMask())
    ol = z.Outline()
    ol.NewOutline()
    for x, y in [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]:
        ol.Append(pcbnew.FromMM(x), pcbnew.FromMM(y))
    b.Add(z)
    return z


def _pads(b, ref, net_names, x, y0, pitch=0.6):
    """A column of SMD pads (0.8 x 0.4 mm) on F.Cu at x, each on its own net."""
    fp = pcbnew.FOOTPRINT(b)
    fp.SetReference(ref)
    b.Add(fp)
    for k, name in enumerate(net_names):
        net = b.FindNet(name)
        if net is None:
            net = pcbnew.NETINFO_ITEM(b, name)
            b.Add(net)
        p = pcbnew.PAD(fp)
        p.SetNumber(str(k + 1))
        p.SetShape(pcbnew.PAD_SHAPE_RECTANGLE)
        p.SetAttribute(pcbnew.PAD_ATTRIB_SMD)
        p.SetSize(pcbnew.VECTOR2I(pcbnew.FromMM(0.8), pcbnew.FromMM(0.4)))
        ls = pcbnew.LSET()
        ls.AddLayer(pcbnew.F_Cu)
        p.SetLayerSet(ls)
        p.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y0 + k * pitch)))
        p.SetNet(net)
        fp.Add(p)


# Corridor 1 (y < 30): 8 nets cross a vertical keep-out wall through a gap. Corridor 2 (y > 30):
# nets run along a band between two horizontal keep-out walls. A wall at y = 30 separates them.
WALL_X = (29.0, 31.0)
GAP_Y = (13.0, 15.0)             # corridor 1's gap
BAND_Y = (44.0, 46.0)            # corridor 2's band
N1, N2 = 8, 6


def _bus(b, prefix, n, y_mid):
    names = [f"{prefix}{k}" for k in range(n)]
    y0 = y_mid - (n - 1) * 0.3
    _pads(b, f"L{prefix}", names, 10.0, y0)
    _pads(b, f"R{prefix}", names, 50.0, y0)


def synthetic():
    """A board that routes: bus A through a 2 mm gap in a wall (capacity about 7 tracks per
    layer), bus B down a 2 mm band (the same)."""
    b = _new_board()
    _rect_outline(b, 0, 0, 60, 60)
    _keepout(b, 0, 29.5, 60, 30.5)                       # separates the corridors
    _keepout(b, WALL_X[0], 0, WALL_X[1], GAP_Y[0])       # corridor 1's wall, above the gap
    _keepout(b, WALL_X[0], GAP_Y[1], WALL_X[1], 29.5)    # and below it
    _keepout(b, 19.0, 30.5, 41.0, BAND_Y[0])             # corridor 2: above the band
    _keepout(b, 19.0, BAND_Y[1], 41.0, 60)               # and below it
    _bus(b, "a", N1, 14.0)
    _bus(b, "b", N2, 45.0)
    return b


def _zone_by_box(b, x0, y0, x1, y1):
    for z in b.Zones():
        bx = z.GetBoundingBox()
        if (abs(pcbnew.ToMM(bx.GetX()) - x0) < 1e-3 and abs(pcbnew.ToMM(bx.GetY()) - y0) < 1e-3
                and abs(pcbnew.ToMM(bx.GetRight()) - x1) < 1e-3
                and abs(pcbnew.ToMM(bx.GetBottom()) - y1) < 1e-3):
            return z
    raise KeyError((x0, y0, x1, y1))


def _narrow_band(b):
    """Corridor 1's gap from 2.0 mm to 0.5 mm: capacity about 2 tracks a layer for 8 nets."""
    z = _zone_by_box(b, WALL_X[0], GAP_Y[1], WALL_X[1], 29.5)
    ol = z.Outline()
    ol.RemoveAllContours()
    ol.NewOutline()
    top = GAP_Y[0] + 0.5
    for x, y in [(WALL_X[0], top), (WALL_X[1], top), (WALL_X[1], 29.5), (WALL_X[0], 29.5)]:
        ol.Append(pcbnew.FromMM(x), pcbnew.FromMM(y))


def _block_channel(b):
    """Close corridor 1's gap: a keep-out across it."""
    _keepout(b, WALL_X[0], GAP_Y[0], WALL_X[1], GAP_Y[1])


def _spike(b):
    """Duplicate corridor 2's bus twice over (6 nets become 18 in a band that takes about 14)."""
    for rep in ("c", "d"):
        _bus(b, rep, N2, 45.0)


PLANT = {"narrowed gap": _narrow_band, "blocked channel": _block_channel,
         "doubled bus": _spike}


def _y(res: Result) -> list[float]:
    return [edge_where(res.model, e)[3] for _, e in res.over_edges]


# each reason, as a test on the result: its own failure and no other
EXPECT = {
    "narrowed gap": lambda r: not r.blocked and r.overflow > 0 and _y(r) and max(_y(r)) < 29.5,
    "blocked channel": lambda r: bool(r.blocked) and {n for n, _, _ in r.blocked} <= {
        f"a{k}" for k in range(N1)},
    "doubled bus": lambda r: not r.blocked and r.overflow > 0 and _y(r) and min(_y(r)) > 30.5,
}


def selftest() -> int:
    base = analyze(synthetic())
    if problems(base):
        print("selftest: the synthetic baseline does not pass:\n  " + "\n  ".join(problems(base)))
        return 1
    failed = 0
    for name, plant in PLANT.items():
        b = synthetic()
        plant(b)
        res = analyze(b)
        for other, test in EXPECT.items():
            hit = bool(test(res))
            if other == name and not hit:
                print(f"selftest: planted fault '{name}' did not fail as {name!r}: "
                      f"{problems(res)[:3]}")
                failed += 1
            if other != name and hit:
                print(f"selftest: planted fault '{name}' also produced the failure of '{other}'")
                failed += 1
    if failed:
        return 1
    print("HW CONGESTION SELFTEST OK")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("board", type=Path, nargs="?")
    ap.add_argument("--rounds", type=int, default=ROUNDS)
    ap.add_argument("--heatmap", type=Path, help="write the utilisation PNG here")
    ap.add_argument("--worst", type=int, default=8)
    ap.add_argument("--selftest", action="store_true",
                    help="plant a narrowed gap, a blocked channel and a doubled bus in copies "
                    "of a synthetic board")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if a.board is None:
        ap.error("BOARD is required")
    board = pcbnew.LoadBoard(str(a.board))
    res = analyze(board, a.rounds)
    m = res.model
    print(f"hw-congestion: {a.board}: {board.GetCopperLayerCount()} layers, grid {CELL} mm "
          f"({m.nx} x {m.ny}), {len(m.nets)} nets routed, planes {sorted(m.planes) or '-'}, "
          f"pours (excluded) {sorted(m.pours) or '-'}")
    print(f"  {res.rounds} negotiation rounds, {res.vias} vias, tree {res.wirelength:.0f} mm, "
          f"overflow {res.overflow:.1f} tracks, {res.seconds:.1f} s")
    if a.heatmap:
        write_heatmap(res, a.heatmap)
        print(f"  heat map: {a.heatmap}")
    found = problems(res, a.worst)
    for p in found:
        print(f"HW CONGESTION: {p}")
    if found:
        return 1
    print("HW CONGESTION OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
