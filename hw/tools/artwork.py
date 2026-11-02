# SPDX-License-Identifier: GPL-3.0-or-later
"""artwork: the carrier's silkscreen art, applied to the routed board without touching copper.

    python3 -m hw.tools.artwork BOARD.kicad_pcb ARTWORK.toml     # saves BOARD

hw/carrier/artwork.toml (a design file) declares every item and its place:

* [name]   one line of stroke-font text,
* [logo]   the open-source-hardware gear, copied from a KiCad library footprint's silkscreen
           polygons (as plain board graphics, not a footprint),
* [grid] + [[ascii]]  ASCII art, one PCB_TEXT per non-space character on a fixed grid, since
           KiCad's stroke font is proportional and a fixed pitch keeps the art aligned with no
           font dependency.

Every item goes into one PCB_GROUP named `gsnix-artwork`. Applying the art first deletes that
group and its items, so it is idempotent. Then the reference designators are placed around it
by rule (hw/tools/silk.py), clear of pads, other silkscreen and the board edge; the few with no
clear place are hidden and listed. It adds no footprints (netlist parity, BOM and CPL are
unchanged) and touches no tracks, vias or zones. Run it after `board`, which makes a board
with no art, and after `route`.
"""
import functools
import os
import re
import shutil
import sys
import tempfile
import tomllib
from pathlib import Path

import pcbnew

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from hw.tools import silk

GROUP = "gsnix-artwork"
LAYER = pcbnew.F_SilkS


def _mm(v: float) -> int:
    return pcbnew.FromMM(v)


def _vec(x: float, y: float) -> pcbnew.VECTOR2I:
    return pcbnew.VECTOR2I(_mm(x), _mm(y))

def _borrow(it):
    """KiCad's Python binding hands out owning wrappers for board items (a temporary one deletes
    the board's item when collected); mark `it` as borrowed so it never does."""
    it.thisown = False
    return it


def group_items(grp) -> list:
    """A group's items, borrowed."""
    return [_borrow(it) for it in grp.GetItems()]


def art_groups(board: pcbnew.BOARD) -> list:
    """The board's `gsnix-artwork` groups, borrowed (and found by index, with the container kept
    alive: iterating a temporary `board.Groups()` crashes KiCad's Python binding)."""
    gs = board.Groups()
    return [_borrow(gs[i]) for i in range(len(gs)) if _borrow(gs[i]).GetName() == GROUP]


def remove_art(board: pcbnew.BOARD) -> None:
    """Delete every `gsnix-artwork` group with its items."""
    for grp in art_groups(board):
        items = group_items(grp)
        # Ungroup, then drop the group, then the items: a removed item that the group still
        # lists would leave the group holding a dangling pointer.
        for it in items:
            grp.RemoveItem(it)
        board.Remove(grp)
        _borrow(grp)            # Remove hands ownership to Python; freeing them crashes, so leak them
        for it in items:
            board.Remove(it)
            _borrow(it)


def _text(board, grp, s: str, x: float, y: float, w: float, h: float, thick: float) -> None:
    t = pcbnew.PCB_TEXT(board)
    t.SetLayer(LAYER)
    t.SetText(s)
    t.SetTextSize(pcbnew.VECTOR2I(_mm(w), _mm(h)))
    t.SetTextThickness(_mm(thick))
    t.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_CENTER)
    t.SetVertJustify(pcbnew.GR_TEXT_V_ALIGN_CENTER)
    t.SetPosition(_vec(x, y))
    board.Add(t)
    t.thisown = False       # the board owns it now; Python must not free it
    grp.AddItem(t)


@functools.cache
def _library_polygons(fpid: str) -> tuple:
    """The filled silkscreen polygons of `Lib:Name` from KiCad's footprint library, as plain
    data: ((width_nm, ((ring,),)), ...) with ring = ((x_nm, y_nm), ...) (the library's logos are
    hole-free polygons). Read from the .kicad_mod text: KiCad's FootprintLoad binding is
    unreliable in one process with board edits (it fails, or crashes, after a Remove)."""
    lib, name = fpid.split(":")
    root = os.environ.get("KICAD10_FOOTPRINT_DIR")
    if not root:
        sys.exit("artwork: KICAD10_FOOTPRINT_DIR is unset (run inside `nix develop .#hw`)")
    path = Path(root) / f"{lib}.pretty" / f"{name}.kicad_mod"
    if not path.is_file():
        sys.exit(f"artwork: footprint {fpid} not found ({path})")
    out = []
    for blk in re.split(r"\(fp_poly\b", path.read_text())[1:]:
        blk = blk.split("(uuid")[0]
        if '(layer "F.SilkS")' not in blk:
            continue
        ring = tuple((_mm(float(x)), _mm(float(y)))
                     for x, y in re.findall(r"\(xy\s+(\S+)\s+(\S+)\)", blk))
        width = re.search(r"\(stroke\s*\(width\s+(\S+?)\)", blk)
        out.append((_mm(float(width.group(1))) if width else 0, ((ring,),)))
    if not out:
        sys.exit(f"artwork: {fpid} has no silkscreen polygons")
    return tuple(out)




def _logo(board, grp, spec: dict) -> None:
    """Copy the silkscreen polygons of a library footprint, centred on (x, y), as board items."""
    shapes = _library_polygons(spec["footprint"])
    xs = [x for _, polys in shapes for rings in polys for x, _ in rings[0]]
    ys = [y for _, polys in shapes for rings in polys for _, y in rings[0]]
    dx = _mm(spec["x_mm"]) - (min(xs) + max(xs)) // 2
    dy = _mm(spec["y_mm"]) - (min(ys) + max(ys)) // 2
    for width, polys in shapes:
        ps = pcbnew.SHAPE_POLY_SET()
        for rings in polys:
            o = ps.NewOutline()
            for k, ring in enumerate(rings):
                if k:
                    ps.NewHole(o)
                for x, y in ring:
                    ps.Append(x + dx, y + dy, o, k - 1 if k else -1)
        c = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_POLY)
        c.SetPolyShape(ps)
        c.SetFilled(True)
        c.SetWidth(width)
        c.SetLayer(LAYER)
        board.Add(c)
        c.thisown = False
        grp.AddItem(c)


def apply(board: pcbnew.BOARD, art: Path) -> list[str]:
    """The art, then the reference designators placed around it (silk.place_references).
    Returns the references hidden for want of a clear place."""
    spec = tomllib.loads(Path(art).read_text())
    _library_polygons(spec["logo"]["footprint"])   # fail early, before touching the board
    remove_art(board)
    grp = pcbnew.PCB_GROUP(board)
    grp.SetName(GROUP)
    board.Add(grp)
    grp.thisown = False

    n = spec["name"]
    _text(board, grp, n["text"], n["x_mm"], n["y_mm"], n["height_mm"], n["height_mm"],
          n["thickness_mm"])
    _logo(board, grp, spec["logo"])

    g = spec["grid"]
    for blk in spec["ascii"]:
        for r, line in enumerate(blk["art"].strip("\n").split("\n")):
            for c, ch in enumerate(line):
                if ch != " ":
                    _text(board, grp, ch, blk["x_mm"] + c * g["col_pitch_mm"],
                          blk["y_mm"] + r * g["row_pitch_mm"], g["width_mm"], g["height_mm"],
                          g["thickness_mm"])
    return silk.place_references(board)


def main(argv: list[str] | None = None) -> int:
    a = sys.argv[1:] if argv is None else argv
    if len(a) != 2:
        sys.exit(__doc__.splitlines()[0] + "\nusage: python3 -m hw.tools.artwork BOARD ARTWORK_TOML")
    src = Path(a[0])
    # On a temporary copy: loading and saving a board rewrites its .kicad_pro.
    with tempfile.TemporaryDirectory() as tmp:
        copy = Path(tmp) / src.name
        shutil.copy(src, copy)
        pro = src.with_suffix(".kicad_pro")
        if pro.exists():
            shutil.copy(pro, copy.with_suffix(".kicad_pro"))
        board = pcbnew.LoadBoard(str(copy))
        hidden = apply(board, Path(a[1]))
        board.Save(str(copy))
        n = len(group_items(art_groups(board)[0]))
        shutil.copy(copy, src)
    print(f"references hidden on the silkscreen (no clear place): {' '.join(hidden) or 'none'}")
    print(f"HW ARTWORK OK ({n} items)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
