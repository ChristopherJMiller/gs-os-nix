# SPDX-License-Identifier: GPL-3.0-or-later
"""Route the carrier with Freerouting, through KiCad's pcbnew.

    python3 hw/tools/route.py BOARD.kicad_pcb [--passes N] [--timeout S] [--keep DIR]

pcbnew exports the Specctra DSN (it carries the net classes of BOARD's .kicad_pro, so
the Power class reaches the router), freerouting routes it, pcbnew imports the SES, and
what the router left is finished here (finish_unrouted); then the pours are refilled and
BOARD is saved in place. Existing tracks and vias are dropped first, so a route is a fresh
route; the zones and all else stay. Freerouting's output is not reproducible, which is why
the routed board is committed: hw-drc (check_drc.py) is what ties it to the netlist.

GND is routed like every other net, in the Power class, and the pours add to it. (Left to
the pours alone, a GND pad that signal tracks have walled in is not connected, and so are
the pieces of pour around it. Routed first or last, GND takes room from the signals or finds
none; routed with them, the router trades the two.)

What is fixed in the DSN between pcbnew and freerouting (prepare_dsn):

* Edge clearance (ruling R4). Freerouting 2.4.1 rejects
  `--router.copperToEdgeClearanceUm 500` ("Unknown command line argument"), so the
  0.5 mm copper-to-edge rule is built into the DSN: a keepout strip along every board
  edge (except the tab's bottom edge, where the bevel keepout already is), as wide as
  the 0.5 mm less the clearance the router adds around a keepout.
* Net names. Freerouting folds case, and the carrier has nets that differ only by case
  (SLOT_D3 on the slot side, slot_d3 on the FPGA side, joined by a translator). Folded,
  they are one net and the router shorts them. The later twin is renamed for the router
  and renamed back in the session (restore_ses).
* The `smd_smd` clearance KiCad exports (a quarter of the real one) is dropped, and the
  clearance is 0.17 mm, not 0.15: the router squeezes a track to 0.112 mm to leave a
  fine-pitch pad, and widening it to the 0.15 mm minimum afterwards (widen_stubs) takes
  its margin.
* No planes: a plane tells the router that its pins are joined already, which pours cut
  up by tracks are not.

finish_unrouted is a small grid router (A*, two layers, vias) for the connections that
freerouting gives up on. It joins the pieces of a net that the router left apart, keeping
0.15 mm from other nets and 0.5 mm from the edge, and never writes a net onto a track
except that of the pieces it joins.
"""
import argparse
import heapq
import math
import os
import re
import signal
import subprocess
import tempfile
from pathlib import Path

import numpy as np
import pcbnew
from PIL import Image, ImageDraw, ImageFilter

EDGE_CLEARANCE_UM = 500
ROUTER_CLEARANCE_UM = 170          # what freerouting keeps (see prepare_dsn)
MIN_TRACK_MM = 0.15
CLEARANCE_MM = 0.15
EDGE_CLEARANCE_MM = 0.5
VIA_MM = 0.6
VIA_DRILL_MM = 0.3
DUP_SUFFIX = "_dup"
GND_JOIN_MM = 0.15                 # width of the GND tracks finish_unrouted lays (hw-drc exempts GND)
RES_MM = 0.05                      # finish_unrouted's grid
MARGIN_MM = 0.03                   # what it keeps over the minimum
HOLE_CLEARANCE_MM = 0.25           # copper to hole wall, KiCad's default
VIA_COST = 12                      # in grid steps
HEURISTIC_WEIGHT = 1.3             # a little greedy: paths a little long, found sooner
MAX_EXPANSIONS = 3_000_000

# A token, quoted or not. (The DSN's `(string_quote ")` has a lone quote, so a quoted
# token may hold no space or parenthesis, which no net name does.)
_TOKEN = re.compile(r'"[^"\s()]*"|[^\s()"]+')


# --- the board --------------------------------------------------------------------------

def strip_routing(board, keep_locked: bool = False, keep_all: bool = False) -> None:
    for t in list(board.GetTracks()):
        if not (keep_all or (keep_locked and t.IsLocked())):
            board.RemoveNative(t)


def solid_zones(board) -> None:
    """GND pads join the pours solidly. Thermal spokes starve between the tracks of a board
    this dense (DRC starved_thermal), and nothing here is hand-soldered through a pour."""
    for z in board.Zones():
        if not z.GetIsRuleArea():
            z.SetPadConnection(pcbnew.ZONE_CONNECTION_FULL)
            # Copper that touches nothing is not a connection to chase: drop it.
            z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)


def widen_stubs(board) -> int:
    """Freerouting narrows a track to squeeze it into a fine-pitch pad (112 um). Make each
    such stub the minimum width; DRC then says whether it still fits."""
    n = 0
    for t in board.GetTracks():
        if t.GetClass() != "PCB_VIA" and t.GetWidth() < pcbnew.FromMM(MIN_TRACK_MM):
            t.SetWidth(pcbnew.FromMM(MIN_TRACK_MM))
            n += 1
    return n


def refill(board) -> None:
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())


# --- DSN preparation ------------------------------------------------------------------

def net_names(dsn: str) -> list[str]:
    return [m.strip('"') for m in re.findall(r'\(net\s+("[^"]*"|[^\s()]+)', dsn)]


def case_twins(names: list[str]) -> dict[str, str]:
    """Renames that make every net name unique when case is folded."""
    groups: dict[str, list[str]] = {}
    for n in sorted(set(names)):
        groups.setdefault(n.lower(), []).append(n)
    rename = {}
    for group in groups.values():
        for i, n in enumerate(group[1:], 1):
            rename[n] = f"{n}{DUP_SUFFIX}{i}"
    if set(rename.values()) & set(names):
        raise SystemExit("route: a renamed net collides with an existing one")
    return rename


def _retoken(text: str, mapping: dict[str, str]) -> str:
    def sub(m):
        tok = m.group(0)
        bare = tok.strip('"')
        if bare in mapping:
            return f'"{mapping[bare]}"' if tok.startswith('"') else mapping[bare]
        return tok
    return _TOKEN.sub(sub, text)


def boundary_points(dsn: str) -> list[tuple[int, int]]:
    m = re.search(r"\(boundary\s*\(path\s+\S+\s+\d+\s+([-\d.\s]+)\)", dsn)
    if not m:
        raise SystemExit("route: no boundary path in the DSN")
    v = [int(float(x)) for x in m.group(1).split()]
    pts = list(zip(v[0::2], v[1::2]))
    return pts[:-1] if pts[0] == pts[-1] else pts


def edge_strips(pts: list[tuple[int, int]], width: int) -> list[list[tuple[int, int]]]:
    """Rectangles `width` wide just inside each outline edge, but the bottom edge of the
    tab (the lowest y in DSN units, where y grows upward)."""
    area = sum(x0 * y1 - x1 * y0
               for (x0, y0), (x1, y1) in zip(pts, pts[1:] + pts[:1]))
    ccw = area > 0
    low = min(y for _, y in pts)
    out = []
    for (x0, y0), (x1, y1) in zip(pts, pts[1:] + pts[:1]):
        if y0 == y1 == low:
            continue
        dx, dy = (x1 - x0), (y1 - y0)
        n = max(abs(dx), abs(dy))
        ux, uy = dx // n, dy // n                       # unit step along the edge
        ix, iy = (-uy, ux) if ccw else (uy, -ux)        # inward normal
        a = (x0 - ux * width, y0 - uy * width)          # extended past the corners
        b = (x1 + ux * width, y1 + uy * width)
        out.append([a, b, (b[0] + ix * width, b[1] + iy * width),
                    (a[0] + ix * width, a[1] + iy * width)])
    return out


def prepare_dsn(dsn: str) -> tuple[str, dict[str, str]]:
    """The DSN freerouting gets, and the net renames (new -> old) to undo in the SES."""
    dsn = re.sub(r"\n[ \t]*\(plane \S+\s+\(polygon[^)]*\)\)", "", dsn)
    rename = case_twins(net_names(dsn))
    dsn = _retoken(dsn, rename)
    dsn = re.sub(r"\s*\(clearance\s+[\d.]+\s+\(type smd_smd\)\)", "", dsn)
    dsn = dsn.replace("(clearance 150)", f"(clearance {ROUTER_CLEARANCE_UM})")
    # A class that wants more (Clock: 0.4 mm) gets only that much here: its pads sit 0.15 mm
    # from their neighbours, so the router would see them as violations it cannot clear.
    # What the clock needs is given back by halo_wires, round the wires that are laid.
    dsn = re.sub(r"\(clearance (\d+)\)",
                 lambda m: f"(clearance {min(int(m.group(1)), ROUTER_CLEARANCE_UM)})", dsn)
    strip = EDGE_CLEARANCE_UM - ROUTER_CLEARANCE_UM
    lines = []
    for poly in edge_strips(boundary_points(dsn), strip):
        coords = "  ".join(f"{x} {y}" for x, y in poly + poly[:1])
        for layer in ("F.Cu", "B.Cu"):
            lines.append(f'(keepout "" (polygon {layer} 0  {coords}))')
    text = "".join(f"    {line}\n" for line in lines)
    i = dsn.index("    (via ")
    return dsn[:i] + text + dsn[i:], {v: k for k, v in rename.items()}


HALO_FROM_PAD_MM = 0.9             # no halo where a track is this near a pad of its own net
HALO_PIECE_MM = 0.5


def halo_wires(board, names: list[str]) -> str:
    """DSN lines that make freerouting keep the clearance of the nets in `names`, where
    that is more than it keeps (ROUTER_CLEARANCE_UM): for each locked track, fat fixed
    wires (width + twice the difference) in pieces, none near a pad, whose pads sit too
    close to neighbours for a wide berth; and, for each via, a keepout circle. The wires
    are for the router only; the board keeps the real ones."""
    lines = []
    um = lambda v: int(round(v / 1000))
    for name in names:
        extra = net_clearance_mm(board, name) - ROUTER_CLEARANCE_UM / 1000
        if extra <= 0:
            continue
        pads = [p.GetBoundingBox() for fp in board.GetFootprints() for p in fp.Pads()
                if p.GetNetname() == name]
        for bb in pads:
            bb.Inflate(pcbnew.FromMM(HALO_FROM_PAD_MM))
        for t in board.GetTracks():
            if t.GetNetname() != name:
                continue
            if t.GetClass() == "PCB_VIA":
                d = um(t.GetWidth(pcbnew.F_Cu)) + 2 * int(round(extra * 1000))
                x, y = um(t.GetPosition().x), -um(t.GetPosition().y)
                for layer in ("F.Cu", "B.Cu"):
                    lines.append(f'(keepout "" (circle {layer} {d} {x} {y}))')
                continue
            a, b = t.GetStart(), t.GetEnd()
            n = max(1, math.ceil(_mm(int((b - a).EuclideanNorm())) / HALO_PIECE_MM))
            w = um(t.GetWidth()) + 2 * int(round(extra * 1000))
            layer = "F.Cu" if t.GetLayer() == pcbnew.F_Cu else "B.Cu"
            for k in range(n):
                p0 = pcbnew.VECTOR2I(a.x + (b.x - a.x) * k // n, a.y + (b.y - a.y) * k // n)
                p1 = pcbnew.VECTOR2I(a.x + (b.x - a.x) * (k + 1) // n,
                                     a.y + (b.y - a.y) * (k + 1) // n)
                mid = pcbnew.VECTOR2I((p0.x + p1.x) // 2, (p0.y + p1.y) // 2)
                if any(bb.Contains(mid) for bb in pads):
                    continue
                lines.append(f"(wire (path {layer} {w}  {um(p0.x)} {-um(p0.y)}  "
                             f"{um(p1.x)} {-um(p1.y)})(net {name})(type fix))")
    return "".join(f"    {l}\n" for l in lines)


def drop_net(dsn: str, name: str) -> str:
    """The DSN without net `name`: its `(net ...)` block, and its entry in the class lists.
    Its pins stay, as copper no net is joined to, so the router keeps clear of them. (GND
    is left to the pours and to finish_unrouted: routed as a net it takes 0.5 mm tracks
    across the back layer and chops the pour.)"""
    m = re.search(r'\(net\s+"?' + re.escape(name) + r'"?\s', dsn)
    if m:
        depth, i = 0, m.start()
        while True:
            depth += {"(": 1, ")": -1}.get(dsn[i], 0)
            i += 1
            if depth == 0:
                break
        dsn = dsn[:m.start()].rstrip(" \t") + dsn[i:].lstrip(" \t")
    word = re.compile(r'(?<![\w+\-.])"?' + re.escape(name) + r'"?(?![\w+\-.])')
    return re.sub(r"\(class [^()]*", lambda mm: word.sub("", mm.group(0)), dsn)


def restore_ses(ses: str, back: dict[str, str]) -> str:
    return _retoken(ses, back)


# --- finishing what the router left -----------------------------------------------------

def _mm(v: int) -> float:
    return pcbnew.ToMM(v)


_LAYERS = (pcbnew.F_Cu, pcbnew.B_Cu)


def _fills(board, net: int) -> dict[int, "pcbnew.SHAPE_POLY_SET"]:
    """The filled copper of the pour of `net`, per layer (empty if it has none)."""
    out = {}
    for z in board.Zones():
        if not z.GetIsRuleArea() and z.GetNetCode() == net:
            for layer in _LAYERS:
                if z.IsOnLayer(layer):
                    out[layer] = z.GetFilledPolysList(layer)
    return out


def net_clearance_mm(board, name: str) -> float:
    """The clearance of a net's class (the Clock class wants 0.4 mm), never less than the
    minimum."""
    nc = board.GetDesignSettings().m_NetSettings.GetEffectiveNetClass(name)
    return max(_mm(nc.GetClearance()), CLEARANCE_MM)


def _components(board, net: int) -> list[dict]:
    """The pieces of `net`, joined by copper that touches: [{pad, trk, via, zone}]. The
    fragments of its pours (zone) count as copper, so a pad in a pour is whole with it."""
    pads = [p for fp in board.GetFootprints() for p in fp.Pads() if p.GetNetCode() == net]
    tracks = [t for t in board.GetTracks() if t.GetNetCode() == net]
    items = [("pad", p) for p in pads] + [("trk" if t.GetClass() != "PCB_VIA" else "via", t)
                                          for t in tracks]
    for layer, poly in _fills(board, net).items():
        for k in range(poly.OutlineCount()):
            items.append(("zone", (layer, k, poly)))
    parent = list(range(len(items)))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    def layers(kind, o):
        if kind == "via":
            return set(_LAYERS)
        if kind == "zone":
            return {o[0]}
        if kind == "pad":
            return {l for l in _LAYERS if o.IsOnLayer(l)}
        return {o.GetLayer()}

    def points(kind, o):
        if kind == "trk":
            return [o.GetStart(), o.GetEnd()]
        return [o.GetPosition()]

    def touches(a, b):
        (ka, oa), (kb, ob) = a, b
        if ka == kb == "zone" or not layers(ka, oa) & layers(kb, ob):
            return False
        if "zone" in (ka, kb):
            (_, zo), (ko, oo) = ((ka, oa), (kb, ob)) if ka == "zone" else ((kb, ob), (ka, oa))
            _, k, poly = zo
            return any(poly.Contains(pt, k) for pt in points(ko, oo))
        for (k1, o1), (k2, o2) in (((ka, oa), (kb, ob)), ((kb, ob), (ka, oa))):
            for pt in points(k1, o1):
                if k2 in ("pad", "trk") and o2.HitTest(pt):
                    return True
                if k2 == "via" and (pt - o2.GetPosition()).EuclideanNorm() <= o2.GetWidth(
                        pcbnew.F_Cu) / 2:
                    return True
        return False

    for i in range(len(items)):
        for j in range(i + 1, len(items)):
            if find(i) != find(j) and touches(items[i], items[j]):
                parent[find(i)] = find(j)
    groups: dict[int, dict] = {}
    for i, (kind, o) in enumerate(items):
        g = groups.setdefault(find(i), {"pad": [], "trk": [], "via": [], "zone": []})
        g[kind].append(o)
    return list(groups.values())


def _size(comp: dict) -> int:
    return len(comp["pad"]) + len(comp["trk"]) + len(comp["via"]) + 5 * len(comp["zone"])


class _Grid:
    """The two copper layers of the board as bitmaps, RES_MM per cell: where a track of
    width `width` of net `net` may not have its centre, and where a via may not."""

    def __init__(self, board, net: int, width_mm: float, ignore_tracks=(),
                 clearance_mm: float = CLEARANCE_MM, pad_clearance_mm: float | None = None):
        self.clearance = clearance_mm
        # Clock-class clearance does not apply to pads (see hw/carrier/carrier.kicad_dru):
        # the module socket's pads are 0.15 mm apart.
        self.pad_delta = clearance_mm - (CLEARANCE_MM if pad_clearance_mm is None
                                         else pad_clearance_mm)
        box = board.GetBoardEdgesBoundingBox()
        self.x0, self.y0 = box.GetLeft(), box.GetTop()
        self.w = int(_mm(box.GetWidth()) / RES_MM) + 2
        self.h = int(_mm(box.GetHeight()) / RES_MM) + 2
        self.net, self.width = net, width_mm
        # a net whose class wants more room than ours (the clocks: 0.4 mm) gets it
        self.extra = {n.GetNetCode(): max(0.0, net_clearance_mm(board, n.GetNetname())
                                          - clearance_mm)
                      for n in board.GetNetsByName().values() if n.GetNetCode()}
        self.ignore = set(ignore_tracks)          # nets whose tracks are not in the way
        trk = clearance_mm + width_mm / 2 + MARGIN_MM
        via = clearance_mm + VIA_MM / 2 + MARGIN_MM
        self.track = {l: self._copper(board, l, trk) for l in _LAYERS}
        via_maps = [self._copper(board, l, via) for l in _LAYERS]
        self.via = via_maps[0] | via_maps[1]
        for l in self.track:
            self.track[l] |= self._outside(board, EDGE_CLEARANCE_MM + width_mm / 2 + MARGIN_MM)
        self.via |= self._outside(board, EDGE_CLEARANCE_MM + VIA_MM / 2 + MARGIN_MM)

    def px(self, x_nm: float, y_nm: float) -> tuple[float, float]:
        return ((x_nm - self.x0) / 1e6 / RES_MM, (y_nm - self.y0) / 1e6 / RES_MM)

    def centre(self, i: int, j: int):
        return pcbnew.VECTOR2I(int(self.x0 + (i + 0.5) * RES_MM * 1e6),
                               int(self.y0 + (j + 0.5) * RES_MM * 1e6))

    def _new(self):
        img = Image.new("L", (self.w, self.h), 0)
        return img, ImageDraw.Draw(img)

    def _pad_outlines(self, pad, layer):
        poly = pcbnew.SHAPE_POLY_SET()
        pad.TransformShapeToPolygon(poly, layer, 0, 10000, pcbnew.ERROR_INSIDE)
        for k in range(poly.OutlineCount()):
            o = poly.Outline(k)
            yield [self.px(o.CPoint(i).x, o.CPoint(i).y) for i in range(o.PointCount())]

    def _copper(self, board, layer: int, grow_mm: float) -> np.ndarray:
        """Cells nearer than `grow_mm` to copper of other nets on `layer`, and to holes
        and keepouts."""
        img, d = self._new()
        g = grow_mm / RES_MM
        for fp in board.GetFootprints():
            for p in fp.Pads():
                if p.GetNetCode() == self.net and self.net != 0:
                    continue
                if p.HasHole():                     # the hole, whatever the layer
                    hr = _mm(max(p.GetDrillSize().x, p.GetDrillSize().y)) / 2 / RES_MM
                    x, y = self.px(p.GetPosition().x, p.GetPosition().y)
                    r = hr + (HOLE_CLEARANCE_MM + grow_mm - self.clearance) / RES_MM
                    d.ellipse([x - r, y - r, x + r, y + r], fill=255)
                if not p.IsOnLayer(layer):
                    continue
                g = (grow_mm - self.pad_delta) / RES_MM
                for pts in self._pad_outlines(p, layer):
                    if len(pts) > 2:
                        d.polygon(pts, fill=255)
                    d.line(pts + pts[:1], fill=255, width=max(1, round(2 * g)), joint="curve")
                    for x, y in pts:
                        d.ellipse([x - g, y - g, x + g, y + g], fill=255)
        for t in board.GetTracks():
            if (t.GetNetCode() == self.net and self.net != 0) or t.GetNetCode() in self.ignore:
                continue
            g = (grow_mm + self.extra.get(t.GetNetCode(), 0.0)) / RES_MM
            if t.GetClass() == "PCB_VIA":
                x, y = self.px(t.GetPosition().x, t.GetPosition().y)
                r = _mm(t.GetWidth(pcbnew.F_Cu)) / 2 / RES_MM + g
                d.ellipse([x - r, y - r, x + r, y + r], fill=255)
            elif t.GetLayer() == layer:
                a, b = self.px(t.GetStart().x, t.GetStart().y), self.px(t.GetEnd().x, t.GetEnd().y)
                r = _mm(t.GetWidth()) / 2 / RES_MM + g
                d.line([a, b], fill=255, width=max(1, round(2 * r)))
                for x, y in (a, b):
                    d.ellipse([x - r, y - r, x + r, y + r], fill=255)
        for z in board.Zones():                       # the bevel keepout
            if z.GetIsRuleArea():
                k = z.GetBoundingBox()
                (xa, ya), (xb, yb) = (self.px(k.GetLeft(), k.GetTop()),
                                      self.px(k.GetRight(), k.GetBottom()))
                d.rectangle([xa - g, ya - g, xb + g, yb + g], fill=255)
        return np.asarray(img, dtype=bool)

    def _outside(self, board, inset_mm: float) -> np.ndarray:
        """Cells nearer than `inset_mm` to the outline, or outside it."""
        outline = pcbnew.SHAPE_POLY_SET()
        board.GetBoardPolygonOutlines(outline, False)
        img, d = self._new()
        for k in range(outline.OutlineCount()):
            o = outline.Outline(k)
            d.polygon([self.px(o.CPoint(i).x, o.CPoint(i).y) for i in range(o.PointCount())],
                      fill=255)
        for _ in range(math.ceil(inset_mm / RES_MM) + 1):   # a square step at a time, and one over
            img = img.filter(ImageFilter.MinFilter(3))
        inside = np.asarray(img, dtype=bool)
        return ~inside

    def own(self, comp: dict) -> dict[int, np.ndarray]:
        """The cells a piece's own copper covers, per layer."""
        out = {}
        for layer in _LAYERS:
            img, d = self._new()
            for p in comp["pad"]:
                if p.IsOnLayer(layer):
                    for pts in self._pad_outlines(p, layer):
                        if len(pts) > 2:
                            d.polygon(pts, fill=255)
            for t in comp["trk"]:
                if t.GetLayer() == layer:
                    a = self.px(t.GetStart().x, t.GetStart().y)
                    b = self.px(t.GetEnd().x, t.GetEnd().y)
                    r = _mm(t.GetWidth()) / 2 / RES_MM
                    d.line([a, b], fill=255, width=max(1, round(2 * r)))
            for v in comp["via"]:
                x, y = self.px(v.GetPosition().x, v.GetPosition().y)
                r = _mm(v.GetWidth(pcbnew.F_Cu)) / 2 / RES_MM
                d.ellipse([x - r, y - r, x + r, y + r], fill=255)
            for lay, k, poly in comp["zone"]:
                if lay != layer:
                    continue
                o = poly.Outline(k)
                d.polygon([self.px(o.CPoint(i).x, o.CPoint(i).y) for i in range(o.PointCount())],
                          fill=255)
                for h in range(poly.HoleCount(k)):
                    o = poly.Hole(k, h)
                    d.polygon([self.px(o.CPoint(i).x, o.CPoint(i).y)
                               for i in range(o.PointCount())], fill=0)
            # one cell in from the edge, so a track that ends on a cell centre ends in the copper
            out[layer] = np.asarray(img.filter(ImageFilter.MinFilter(3)), dtype=bool)
        return out


def _astar(grid: _Grid, src: dict, dst: dict, weight: float = HEURISTIC_WEIGHT,
           limit: int = MAX_EXPANSIONS):
    """Cheapest path from any free cell of `src` to any free cell of `dst` (cell masks per
    layer), as a list of (layer, i, j); None if there is none. Moves: 8 neighbours, or a
    via to the other layer."""
    other = {pcbnew.F_Cu: pcbnew.B_Cu, pcbnew.B_Cu: pcbnew.F_Cu}
    goal = {l: dst[l] & ~grid.track[l] for l in _LAYERS}
    ys = np.concatenate([np.nonzero(goal[l].any(axis=1))[0] for l in _LAYERS])
    xs = np.concatenate([np.nonzero(goal[l].any(axis=0))[0] for l in _LAYERS])
    if len(ys) == 0:
        return None
    gx0, gx1, gy0, gy1 = int(xs.min()), int(xs.max()), int(ys.min()), int(ys.max())

    def h(i, j):
        dx = max(gx0 - i, 0, i - gx1)
        dy = max(gy0 - j, 0, j - gy1)
        return weight * (max(dx, dy) + 0.414 * min(dx, dy))

    heap, best, prev = [], {}, {}
    for l in _LAYERS:
        for j, i in np.argwhere(src[l] & ~grid.track[l]):
            i, j = int(i), int(j)
            best[(l, i, j)] = 0.0
            heapq.heappush(heap, (h(i, j), 0.0, l, i, j))
    steps = [(1, 0, 1.0), (-1, 0, 1.0), (0, 1, 1.0), (0, -1, 1.0),
             (1, 1, 1.414), (1, -1, 1.414), (-1, 1, 1.414), (-1, -1, 1.414)]
    n = 0
    while heap and n < limit:
        _, g, l, i, j = heapq.heappop(heap)
        if g > best.get((l, i, j), 1e18):
            continue
        n += 1
        if goal[l][j, i]:
            path = [(l, i, j)]
            while path[-1] in prev:
                path.append(prev[path[-1]])
            return path[::-1]
        moves = [(l, i + di, j + dj, c) for di, dj, c in steps]
        if not grid.via[j, i]:
            moves.append((other[l], i, j, VIA_COST))
        for nl, ni, nj, c in moves:
            if not (0 <= ni < grid.w and 0 <= nj < grid.h) or grid.track[nl][nj, ni]:
                continue
            if nl == l and ni != i and nj != j and (grid.track[l][j, ni] or grid.track[l][nj, i]):
                continue                                  # no squeezing through a corner
            ng = g + c
            if ng < best.get((nl, ni, nj), 1e18):
                best[(nl, ni, nj)] = ng
                prev[(nl, ni, nj)] = (l, i, j)
                heapq.heappush(heap, (ng + h(ni, nj), ng, nl, ni, nj))
    return None


def _lay_path(board, grid: _Grid, net: int, path, width_mm: float) -> list:
    """Tracks along `path`, merged where straight, and a via at each change of layer.
    Returns what it added."""
    nets = board.FindNet(net)
    made = []
    i = 0
    while i < len(path) - 1:
        layer, ci, cj = path[i]
        if path[i + 1][0] != layer:
            v = pcbnew.PCB_VIA(board)
            v.SetPosition(grid.centre(ci, cj))
            v.SetViaType(pcbnew.VIATYPE_THROUGH)
            v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
            v.SetWidth(pcbnew.FromMM(VIA_MM))
            v.SetDrill(pcbnew.FromMM(VIA_DRILL_MM))
            v.SetNet(nets)
            board.Add(v)
            made.append(v)
            i += 1
            continue
        d = (path[i + 1][1] - ci, path[i + 1][2] - cj)
        k = i + 1
        while k + 1 < len(path) and path[k + 1][0] == layer and \
                (path[k + 1][1] - path[k][1], path[k + 1][2] - path[k][2]) == d:
            k += 1
        t = pcbnew.PCB_TRACK(board)
        t.SetStart(grid.centre(ci, cj))
        t.SetEnd(grid.centre(path[k][1], path[k][2]))
        t.SetLayer(layer)
        t.SetWidth(pcbnew.FromMM(width_mm))
        t.SetNet(nets)
        board.Add(t)
        made.append(t)
        i = k
    return made


def _shortest(grid: _Grid, comps: list[dict], weight: float = HEURISTIC_WEIGHT,
              limit: int = MAX_EXPANSIONS):
    """A path from the smallest piece to the nearest other piece, or None."""
    comps.sort(key=_size)
    src = grid.own(comps[0])
    for other in comps[1:]:
        path = _astar(grid, src, grid.own(other), weight, limit)
        if path:
            return path
    return None


def _join_pieces(board, net: int, width_mm: float, made: list, ignore=(),
                 clearance_mm: float = CLEARANCE_MM, weight: float = HEURISTIC_WEIGHT,
                 limit: int = MAX_EXPANSIONS) -> bool:
    """Join the pieces of `net` one pair at a time until it is whole; False if stuck.
    `made` collects what is added."""
    for _ in range(40):
        comps = _components(board, net)
        if len(comps) < 2:
            return True
        grid = _Grid(board, net, width_mm, ignore, clearance_mm)
        path = _shortest(grid, comps, weight, limit)
        if not path:
            return False
        made += _lay_path(board, grid, net, path, width_mm)
    return False


def _seg_dist(a0, a1, b0, b1) -> float:
    """Distance, in mm, between two segments (points are VECTOR2I)."""
    def pt_seg(p, q0, q1):
        px, py, x0, y0, x1, y1 = p.x, p.y, q0.x, q0.y, q1.x, q1.y
        dx, dy = x1 - x0, y1 - y0
        t = 0.0 if dx == dy == 0 else max(0.0, min(1.0, ((px - x0) * dx + (py - y0) * dy)
                                                    / (dx * dx + dy * dy)))
        return math.hypot(px - (x0 + t * dx), py - (y0 + t * dy))

    def cross(o, p, q):
        return (p.x - o.x) * (q.y - o.y) - (p.y - o.y) * (q.x - o.x)

    if (cross(a0, a1, b0) * cross(a0, a1, b1) < 0) and (cross(b0, b1, a0) * cross(b0, b1, a1) < 0):
        return 0.0
    return _mm(int(min(pt_seg(a0, b0, b1), pt_seg(a1, b0, b1),
                       pt_seg(b0, a0, a1), pt_seg(b1, a0, a1))))


def _shape(t):
    """(start, end, radius in mm) of a track or via."""
    if t.GetClass() == "PCB_VIA":
        return t.GetPosition(), t.GetPosition(), _mm(t.GetWidth(pcbnew.F_Cu)) / 2
    return t.GetStart(), t.GetEnd(), _mm(t.GetWidth()) / 2


def _clashes(t, made) -> bool:
    """Is track or via `t` too near anything in `made` (same layer, or a via)?"""
    a0, a1, ar = _shape(t)
    for n in made:
        via = "PCB_VIA" in (t.GetClass(), n.GetClass())
        if not via and t.GetLayer() != n.GetLayer():
            continue
        b0, b1, br = _shape(n)
        if _seg_dist(a0, a1, b0, b1) - ar - br < CLEARANCE_MM:
            return True
    return False


def _clone(t):
    """A copy of a track or via (Clone() comes back untyped through SWIG)."""
    if t.GetClass() == "PCB_VIA":
        c = pcbnew.PCB_VIA(t.GetBoard())
        c.SetPosition(t.GetPosition())
        c.SetViaType(pcbnew.VIATYPE_THROUGH)
        c.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
        c.SetWidth(t.GetWidth(pcbnew.F_Cu))
        c.SetDrill(t.GetDrill())
    else:
        c = pcbnew.PCB_TRACK(t.GetBoard())
        c.SetStart(t.GetStart())
        c.SetEnd(t.GetEnd())
        c.SetLayer(t.GetLayer())
        c.SetWidth(t.GetWidth())
    c.SetNet(t.GetNet())
    return c


def net_width_mm(board, name: str) -> float:
    """The track width of a net's class (the Power class makes the rails 0.5 mm), never
    less than the minimum."""
    if name == "GND":                   # the pours carry it; a track only joins what they miss
        return GND_JOIN_MM
    nc = board.GetDesignSettings().m_NetSettings.GetEffectiveNetClass(name)
    return max(_mm(nc.GetTrackWidth()), MIN_TRACK_MM)


def _rip_up_and_join(board, net: int, width_mm: float | None = None) -> str | None:
    """`net` is walled in by the tracks of other nets. Take away the tracks of a small net
    (or, failing that, just the GND tracks that are in the way), join `net`, and put what
    was taken away back (the small net routed again; GND by its pours and, where they
    are cut, by new tracks). Returns what it moved, or None (and the board as it was)."""
    gnd = board.FindNet("GND").GetNetCode()
    width_of = (lambda code: width_mm if width_mm else
                net_width_mm(board, board.FindNet(code).GetNetname()))
    pieces = sorted(_components(board, net), key=_size)
    near = set()                                  # nets with copper close to any piece
    for piece in pieces:
        for kind in ("pad", "trk", "via"):
            for o in piece[kind]:
                box = o.GetBoundingBox()
                box.Inflate(pcbnew.FromMM(2.0))
                near |= {t.GetNetCode() for t in board.GetTracks()
                         if t.GetNetCode() not in (0, net) and box.Intersects(t.GetBoundingBox())}
    pad_count = {}
    for fp in board.GetFootprints():
        for p in fp.Pads():
            pad_count[p.GetNetCode()] = pad_count.get(p.GetNetCode(), 0) + 1
    locked = {t.GetNetCode() for t in board.GetTracks() if t.IsLocked()}
    order = sorted((m for m in near if m != gnd and m not in locked
                    and pad_count.get(m, 0) <= 4),
                   key=lambda m: pad_count.get(m, 0))
    for m in order + ([gnd] if gnd in near else []):
        w, wm = width_of(net), width_of(m)
        grid = _Grid(board, net, w, ignore_tracks={m})
        src = grid.own(pieces[0])
        path = next((p for o in pieces[1:]
                     if (p := _astar(grid, src, grid.own(o)))), None)
        if not path:
            continue
        if m == gnd:                       # only the GND tracks the new path runs into
            planned = _lay_path(board, grid, net, path, w)
            old = [t for t in board.GetTracks()
                   if t.GetNetCode() == m and not t.IsLocked() and _clashes(t, planned)]
            for t in planned:
                board.RemoveNative(t)
        else:
            old = [t for t in board.GetTracks() if t.GetNetCode() == m]
        saved = [_clone(t) for t in old]
        for t in old:
            board.RemoveNative(t)
        made = _lay_path(board, grid, net, path, w)
        if m == gnd:
            refill(board)
        if _join_pieces(board, net, w, made) and _join_pieces(board, m, wm, made):
            return board.FindNet(m).GetNetname()
        for t in made:                                # it did not work: put it all back
            board.RemoveNative(t)
        for t in saved:
            board.Add(t)
        if m == gnd:
            refill(board)
    return None


def finish_unrouted(board, width_mm: float | None = None) -> list[str]:
    """Join the pieces of each net that the router left apart (GND last, with its pours
    taken into account), moving a small net or some GND track aside where one is in the
    way. Every net is laid at the width of its net class (`width_mm` overrides, for
    tests): a rail that cannot be joined at 0.5 mm is reported, not joined thin. Returns
    what could not be joined, as messages."""
    failed = []
    names = sorted({p.GetNetname() for fp in board.GetFootprints() for p in fp.Pads()
                    if p.GetNetCode()} - {"GND"}) + ["GND"]
    refill(board)
    for name in names:
        net = board.FindNet(name).GetNetCode()
        w = width_mm or net_width_mm(board, name)
        if _join_pieces(board, net, w, []):
            continue
        moved = _rip_up_and_join(board, net, width_mm)
        if moved is None:
            failed.append(f"{name}: no path between its pieces at {w:.2f} mm")
        else:
            print(f"finish: {name} joined by moving {moved}")
    return failed


# --- the first pass -------------------------------------------------------------------

# Laid first, in this order, then locked: the clocks (class Clock: 0.2 mm, 0.4 mm clearance)
# and the data bus, on both sides of the translator. They are the nets whose length is
# load on Apple's bus (hw-load) or whose edges must stay clear of the rest.
CLOCK_NETS = ["SLOT_PH0", "SLOT_PH1", "SLOT_Q3", "SLOT_7M",
              "slot_ph0", "slot_ph1", "slot_q3", "slot_7m"]
DATA_NETS = [f"{p}{i}" for i in range(8) for p in ("SLOT_D", "slot_d")]
FIRST_WEIGHT = 1.0                 # A* with no greed: as short as the grid allows


def default_first(board) -> list[str]:
    """CLOCK_NETS then DATA_NETS, those the board has."""
    have = {n.GetNetname() for n in board.GetNetsByName().values() if n.GetNetCode()}
    return [n for n in CLOCK_NETS + DATA_NETS if n in have]


FIRST_LIMIT = 400_000              # expansions before a first-pass net is given up on
FIRST_ROUNDS = 8


def _lay_all(board, names: list[str]) -> tuple[list[str], list]:
    """One attempt: each net of `names` in turn. Returns (the nets that could not be
    joined, every track and via laid for the others); a failed net's partial copper is
    taken away again."""
    failed, laid = [], []
    for name in names:
        net = board.FindNet(name)
        if net is None or net.GetNetCode() == 0:
            failed.append(name)
            continue
        w, c = net_width_mm(board, name), net_clearance_mm(board, name)
        made: list = []
        if _join_pieces(board, net.GetNetCode(), w, made, clearance_mm=c,
                        weight=FIRST_WEIGHT, limit=FIRST_LIMIT):
            laid += made
        else:
            failed.append(name)
            for t in made:
                board.RemoveNative(t)
    return failed, laid


def lay_first(board, names: list[str], rounds: int = FIRST_ROUNDS) -> list[str]:
    """Join the pieces of each net in `names` with the A* finisher at the width and
    clearance of the net's class and the shortest path it finds, and lock what it lays
    (pcbnew exports locked tracks to the DSN as `(type fix)`, which freerouting leaves
    alone). Greedy order can wall a net in, so a net that fails is moved to the front and
    the pass is done again, up to `rounds` times; a net that still fails is left to the
    router (nothing of it is laid). Returns the nets left to the router."""
    order = list(names)
    for r in range(rounds):
        failed, laid = _lay_all(board, order)
        if not failed or r == rounds - 1:
            break
        print(f"first pass round {r + 1}: {', '.join(failed)} did not fit; again, those first")
        for t in laid:
            board.RemoveNative(t)
        order = failed + [n for n in order if n not in failed]
    for t in laid:
        t.SetLocked(True)
    return failed


# --- GND stitching --------------------------------------------------------------------

STITCH_PITCH_MM = 3.0              # <= 5 mm, the plan's limit
STITCH_SEARCH_MM = 1.2             # how far from a grid point a via may be put to find room


def _courtyards(grid: "_Grid", board) -> np.ndarray:
    """Cells under any footprint's courtyard (front or back)."""
    img, d = grid._new()
    for fp in board.GetFootprints():
        for layer in (pcbnew.F_CrtYd, pcbnew.B_CrtYd):
            poly = fp.GetCourtyard(layer)
            for k in range(poly.OutlineCount()):
                o = poly.Outline(k)
                d.polygon([grid.px(o.CPoint(i).x, o.CPoint(i).y) for i in range(o.PointCount())],
                          fill=255)
    return np.asarray(img, dtype=bool)


def stitch_gnd(board, pitch_mm: float = STITCH_PITCH_MM) -> int:
    """GND vias on a grid of `pitch_mm`, each moved a little to where there is room: not
    in a keep-out, not near any pad, track or via of any net (GND's own included), not
    under a courtyard, and inside the filled GND pour on both layers, so each joins the two.
    Only in the bus region: from the middle of the module socket J2 (the band above it is
    for the module and the silkscreen art) down to the fingers. The pours are refilled
    first and after. Returns how many it placed."""
    gnd = board.FindNet("GND").GetNetCode()
    refill(board)
    grid = _Grid(board, 0, MIN_TRACK_MM)               # net 0: every pad and track is in the way
    blocked = grid.via | _courtyards(grid, board)
    pour = {}
    for z in board.Zones():
        if not z.GetIsRuleArea() and z.GetNetCode() == gnd:
            for l in _LAYERS:
                if z.IsOnLayer(l):
                    pour[l] = z.GetFilledPolysList(l)
    if set(pour) != set(_LAYERS):
        raise SystemExit("stitch_gnd: GND has no pour on both layers")
    box = board.GetBoardEdgesBoundingBox()
    j2 = board.FindFootprintByReference("J2")
    y_min = j2.GetPosition().y if j2 else box.GetTop()
    step = round(0.4 / RES_MM)
    reach = int(STITCH_SEARCH_MM / 0.4)
    placed = []
    nx, ny = int(_mm(box.GetWidth()) / pitch_mm) + 1, int(_mm(box.GetHeight()) / pitch_mm) + 1
    offsets = sorted(((dx, dy) for dx in range(-reach, reach + 1) for dy in range(-reach, reach + 1)),
                     key=lambda o: o[0] ** 2 + o[1] ** 2)
    for gy in range(ny):
        for gx in range(nx):
            ci = int((gx * pitch_mm + pitch_mm / 2) / RES_MM)
            cj = int((gy * pitch_mm + pitch_mm / 2) / RES_MM)
            for dx, dy in offsets:
                i, j = ci + dx * step, cj + dy * step
                if grid.centre(i, j).y < y_min:
                    continue
                if not (0 <= i < grid.w and 0 <= j < grid.h) or blocked[j, i]:
                    continue
                pt = grid.centre(i, j)
                if not all(pour[l].Contains(pt) for l in _LAYERS):
                    continue
                if any((pt - q).EuclideanNorm() < _mm_to_nm(pitch_mm * 0.6) for q in placed):
                    continue
                v = pcbnew.PCB_VIA(board)
                v.SetPosition(pt)
                v.SetViaType(pcbnew.VIATYPE_THROUGH)
                v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
                v.SetWidth(pcbnew.FromMM(VIA_MM))
                v.SetDrill(pcbnew.FromMM(VIA_DRILL_MM))
                v.SetNet(board.FindNet(gnd))
                board.Add(v)
                placed.append(pt)
                break
    refill(board)
    return len(placed)


def _mm_to_nm(v: float) -> float:
    return v * 1e6


# --- the run ----------------------------------------------------------------------------

def run_freerouting(dsn: Path, ses: Path, passes: int, timeout_s: int, log: Path,
                    extra: list[str] = ()) -> int:
    """Run freerouting, its output going to `log` as it goes; kill it at the timeout."""
    cmd = ["freerouting", "-de", str(dsn), "-do", str(ses), "-mp", str(passes),
           "--gui.enabled=false", *extra]
    with open(log, "w") as f:
        p = subprocess.Popen(cmd, stdout=f, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            return p.wait(timeout=timeout_s)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            raise SystemExit(f"freerouting still running after {timeout_s} s; see {log}")


def route(board: Path, passes: int = 30, timeout_s: int = 3000, keep: Path | None = None,
          extra: list[str] = (), first: list[str] | None = None,
          router=run_freerouting, stitch: bool = False, keep_locked: bool = False, keep_all: bool = False) -> None:
    """Route `board` in place. `first` (net names; [] for the default set) are laid by the
    finisher before the router sees the board and locked, so the router keeps them as
    fixed wires. `router` is run_freerouting's signature (a test passes a stub)."""
    board = Path(board)
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(keep) if keep else Path(tmp)
        work.mkdir(parents=True, exist_ok=True)
        dsn, ses, log = work / "carrier.dsn", work / "carrier.ses", work / "freerouting.log"
        b = pcbnew.LoadBoard(str(board))
        strip_routing(b, keep_locked, keep_all)
        solid_zones(b)
        names: list[str] = []
        if first is not None:
            names = first or default_first(b)
            bad = lay_first(b, names)
            print(f"first pass: laid and locked {len(names) - len(bad)} of {len(names)} nets"
                  + (f"; left to the router: {', '.join(bad)}" if bad else ""))
        if not pcbnew.ExportSpecctraDSN(b, str(dsn)):
            raise SystemExit(f"pcbnew could not export {dsn}")
        dsn_text = dsn.read_text()
        if names:
            halos = halo_wires(b, names)
            i = dsn_text.index("(wiring") + len("(wiring\n")
            dsn_text = dsn_text[:i] + halos + dsn_text[i:]
        text, back = prepare_dsn(drop_net(dsn_text, "GND"))
        dsn.write_text(text)
        router(dsn, ses, passes, timeout_s, log, extra)
        if not ses.exists():
            raise SystemExit(f"freerouting wrote no session file; see {log}")
        ses.write_text(restore_ses(ses.read_text(), back))
        if not pcbnew.ImportSpecctraSES(b, str(ses)):
            raise SystemExit(f"pcbnew could not import {ses}")
        print(f"widened {widen_stubs(b)} stubs")
        pcbnew.SaveBoard(str(board), b)                 # the router's own result, kept
        left = finish_unrouted(b)
        if stitch:                      # last, into the room that is left: it walls nothing in
            print(f"stitched {stitch_gnd(b)} GND vias")
        refill(b)
        pcbnew.SaveBoard(str(board), b)
    if left:
        raise SystemExit("route: left unrouted:\n  " + "\n  ".join(left))
    print(f"routed {board}")


def main(argv: list[str] | None = None) -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("board", type=Path)
    ap.add_argument("--passes", type=int, default=30)
    ap.add_argument("--timeout", type=int, default=3000, help="seconds")
    ap.add_argument("--keep", type=Path, help="keep the DSN, SES and log in this directory")
    ap.add_argument("--first", nargs="?", const="", default=None, metavar="NETS",
                    help="lay these nets (comma-separated) before freerouting and lock them; "
                    "no value: the clocks, then SLOT_D0-7 and slot_d0-7")
    ap.add_argument("--keep-locked", action="store_true",
                    help="keep the locked tracks of the board (a first pass done before)")
    ap.add_argument("--keep-all", action="store_true",
                    help="keep every track and via: the router continues a route (the GND "
                    "stitching vias of a finished route are not kept: strip them first)")
    ap.add_argument("--stitch", action="store_true",
                    help="after routing, add GND stitching vias (a 4 mm grid) where there is room")
    ap.add_argument("--router-args", default="", help="more arguments for freerouting")
    a = ap.parse_args(argv)
    first = None if a.first is None else [n for n in a.first.split(",") if n]
    route(a.board, a.passes, a.timeout, a.keep, extra=a.router_args.split(), first=first,
          stitch=a.stitch, keep_locked=a.keep_locked,
          keep_all=a.keep_all)


if __name__ == "__main__":
    main()
