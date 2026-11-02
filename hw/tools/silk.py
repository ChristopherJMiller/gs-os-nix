# SPDX-License-Identifier: GPL-3.0-or-later
"""silk: the reference designators on the silkscreen, placed by rule.

    silk.place_references(board) -> the references it hid

artwork.apply runs it after the art, so the art is an obstacle like any other silkscreen. Each
part whose reference is on F.SilkS (every part but J1, the fingers, whose reference is on the
fab layer) gets it placed in turn: first the ICs, connectors and other parts, then the resistors
and capacitors, each group by number. The places tried lie beside the part's anchor (its pads
and its body outline on F.Fab): above and below it with the text level, left and right of it
with the text turned 90 degrees, and the other way round, from PAD_GAP_MM out to MAX_GAP_MM
and slid along that side, nearest first. The first place that is

* PAD_GAP_MM clear of every pad (silkscreen on a pad is clipped by its mask opening: KiCad's
  silk_over_copper),
* SILK_GAP_MM clear of every other silkscreen item (footprint outlines and the art), and
  REF_GAP_MM clear of the references already placed (silk_overlap),
* BODY_GAP_MM clear of every part's body, its own too (it would print under the part),
* EDGE_GAP_MM inside the board outline (silk_edge_clearance),
* and nearer to its own part's anchor than to any other part's, so it names the right part,

wins. A part with no such place has its reference hidden on the silkscreen: the fab layer's
${REFERENCE} and the interactive BOM still name it. The text is the library's, 1 mm high with a
0.15 mm stroke, level or reading upward. Boxes are the glyphs' own (the text's effective shape),
not KiCad's text box, which adds a margin. Nothing depends on where the references were before,
so placing them again changes nothing (hw-drc gates the result: no silkscreen warning).
"""
import re

import numpy as np
import pcbnew

TEXT_MM = 1.0           # the KiCad library's reference size, and JLCPCB's legible minimum
THICKNESS_MM = 0.15
PAD_GAP_MM = 0.2        # to any pad
SILK_GAP_MM = 0.15      # to footprint outlines and the art
REF_GAP_MM = 0.3        # to another reference, so two never run together
BODY_GAP_MM = 0.1       # to a part's body outline (F.Fab)
EDGE_GAP_MM = 0.5       # inside the board outline
MAX_GAP_MM = 1.0        # the farthest a reference may sit from its part's anchor
STEP_MM = 0.1           # the places tried, in both directions
SLIDE_WEIGHT = 0.5      # a place slid along the side costs half its slide
TURN_COST_MM = 0.3      # and turned text costs this much more than level text
SIDES = ("above", "below", "left", "right")
ANGLES = (0, 90)        # level, or turned to read upward


def _box(bb) -> tuple:
    return (pcbnew.ToMM(bb.GetLeft()), pcbnew.ToMM(bb.GetTop()), pcbnew.ToMM(bb.GetRight()),
            pcbnew.ToMM(bb.GetBottom()))


def _shape_box(item, layer) -> tuple:
    return _box(item.GetEffectiveShape(layer).BBox(0))


def _union(boxes) -> tuple:
    return (min(b[0] for b in boxes), min(b[1] for b in boxes), max(b[2] for b in boxes),
            max(b[3] for b in boxes))


def _grow(b, m) -> tuple:
    return b[0] - m, b[1] - m, b[2] + m, b[3] + m


def _pads(fp) -> list:
    return [_shape_box(p, pcbnew.F_Cu) for p in fp.Pads()
            if p.IsOnLayer(pcbnew.F_Cu) or p.IsOnLayer(pcbnew.F_Mask)]


def _body(fp):
    fab = [_shape_box(g, pcbnew.F_Fab) for g in fp.GraphicalItems()
           if g.GetLayer() == pcbnew.F_Fab and g.GetClass() == "PCB_SHAPE"]
    return _union(fab) if fab else None


def anchor(fp) -> tuple:
    """The box a part's reference sits beside: its pads and its body outline (F.Fab), in mm."""
    boxes = _pads(fp) + ([_body(fp)] if _body(fp) else [])
    return _union(boxes) if boxes else _box(fp.GetBoundingBox(False))


def distance(a, x, y) -> float:
    """From the point (x, y) to the box a (0 inside it)."""
    dx = max(a[0] - x, 0.0, x - a[2])
    dy = max(a[1] - y, 0.0, y - a[3])
    return (dx * dx + dy * dy) ** 0.5


def _number(ref: str) -> tuple:
    m = re.fullmatch(r"([A-Za-z]+)(\d+)", ref)
    return (m.group(1), int(m.group(2))) if m else (ref, 0)


def _order(fp) -> tuple:
    ref = fp.GetReference()
    return (ref[:1] in ("R", "C"), _number(ref))


def _obstacles(board) -> np.ndarray:
    """Every box a reference must stay out of, grown by its gap: pads, bodies, the footprints'
    own silkscreen (not their fields) and the board's (the art)."""
    out = []
    for fp in board.GetFootprints():
        out += [_grow(b, PAD_GAP_MM) for b in _pads(fp)]
        body = _body(fp)
        if body:
            out.append(_grow(body, BODY_GAP_MM))
        out += [_grow(_shape_box(g, pcbnew.F_SilkS), SILK_GAP_MM) for g in fp.GraphicalItems()
                if g.GetLayer() == pcbnew.F_SilkS and g.GetClass() != "PCB_FIELD"]
    out += [_grow(_shape_box(d, pcbnew.F_SilkS), SILK_GAP_MM) for d in board.GetDrawings()
            if d.GetLayer() == pcbnew.F_SilkS]
    return np.array(out, dtype=float).reshape(-1, 4)


def _hits(boxes: np.ndarray, b) -> bool:
    return bool(((boxes[:, 0] < b[2]) & (boxes[:, 2] > b[0]) &
                 (boxes[:, 1] < b[3]) & (boxes[:, 3] > b[1])).any())


class _Outline:
    """The board outline, shrunk by EDGE_GAP_MM: a box fits if its corners are inside the
    outline and no outline corner is inside it (the outline is rectilinear)."""

    def __init__(self, board):
        self.poly = pcbnew.SHAPE_POLY_SET()
        if not board.GetBoardPolygonOutlines(self.poly, True):
            raise SystemExit("silk: the board has no closed outline")
        self.corners = [(pcbnew.ToMM(self.poly.CVertex(i).x), pcbnew.ToMM(self.poly.CVertex(i).y))
                        for i in range(self.poly.TotalVertices())]

    def fits(self, b) -> bool:
        x0, y0, x1, y1 = _grow(b, EDGE_GAP_MM)
        if not all(self.poly.Contains(pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y)))
                   for x, y in ((x0, y0), (x1, y0), (x0, y1), (x1, y1))):
            return False
        return not any(x0 < x < x1 and y0 < y < y1 for x, y in self.corners)


def _extents(field, angle) -> tuple:
    """The glyphs' box about the field's position, at `angle`."""
    field.SetTextAngleDegrees(angle)
    field.SetPosition(pcbnew.VECTOR2I(0, 0))
    return _shape_box(field, pcbnew.F_SilkS)


def _places(a, extents: dict):
    """(cost, angle, side, centre x, centre y) beside anchor a, nearest first."""
    out = []
    gaps = [round(PAD_GAP_MM + i * STEP_MM, 4)
            for i in range(int(round((MAX_GAP_MM - PAD_GAP_MM) / STEP_MM)) + 1)]
    for ai, angle in enumerate(ANGLES):
        ex = extents[angle]
        w, h = ex[2] - ex[0], ex[3] - ex[1]
        for si, side in enumerate(SIDES):
            level = side in ("above", "below")
            span = max(a[2] - a[0], w) / 2 if level else max(a[3] - a[1], h) / 2
            mid = (a[0] + a[2]) / 2 if level else (a[1] + a[3]) / 2
            for g in gaps:
                if side == "above":
                    fixed = a[1] - g - h / 2
                elif side == "below":
                    fixed = a[3] + g + h / 2
                elif side == "left":
                    fixed = a[0] - g - w / 2
                else:
                    fixed = a[2] + g + w / 2
                for k in range(int(span / STEP_MM + 1e-9) + 1):
                    for s in ((0,) if k == 0 else (-1, 1)):
                        slide = s * k * STEP_MM
                        x, y = (mid + slide, fixed) if level else (fixed, mid + slide)
                        cost = round(g + SLIDE_WEIGHT * abs(slide)
                                     + (TURN_COST_MM if angle else 0.0), 4)
                        out.append((cost, ai, si, round(x, 4), round(y, 4), angle))
    out.sort()
    return out


def place_references(board) -> list[str]:
    """Place every F.SilkS reference by the rules above; hide those with no place. Returns the
    hidden references, by number."""
    fps = sorted(board.GetFootprints(), key=_order)
    anchors = {fp.GetReference(): anchor(fp) for fp in fps}
    blocked = _obstacles(board)
    outline = _Outline(board)
    placed = np.zeros((0, 4))
    hidden = []
    for fp in fps:
        ref, f = fp.GetReference(), fp.Reference()
        if f.GetLayer() != pcbnew.F_SilkS:
            continue
        f.SetVisible(True)
        f.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(TEXT_MM), pcbnew.FromMM(TEXT_MM)))
        f.SetTextThickness(pcbnew.FromMM(THICKNESS_MM))
        f.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_CENTER)
        f.SetVertJustify(pcbnew.GR_TEXT_V_ALIGN_CENTER)
        a = anchors[ref]
        extents = {angle: _extents(f, angle) for angle in ANGLES}
        others = [o for r, o in anchors.items() if r != ref]
        chosen = None
        for _, _, _, x, y, angle in _places(a, extents):
            ex = extents[angle]
            w, h = ex[2] - ex[0], ex[3] - ex[1]
            b = (x - w / 2, y - h / 2, x + w / 2, y + h / 2)
            if (_hits(blocked, b) or _hits(placed, _grow(b, REF_GAP_MM))
                    or not outline.fits(b)):
                continue
            own = distance(a, x, y)
            if any(distance(o, x, y) <= own for o in others):
                continue
            chosen = (b, angle, ex)
            break
        if chosen is None:
            f.SetVisible(False)
            hidden.append(ref)
            continue
        b, angle, ex = chosen
        f.SetTextAngleDegrees(angle)
        x = float((b[0] + b[2]) / 2 - (ex[0] + ex[2]) / 2)
        y = float((b[1] + b[3]) / 2 - (ex[1] + ex[3]) / 2)
        f.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y)))
        placed = np.vstack([placed, b])
    return sorted(hidden, key=_number)
