# SPDX-License-Identifier: GPL-3.0-or-later
"""The silkscreen art (hw/tools/artwork.py), applied to temporary copies of the routed board.

The art is one PCB_GROUP of silkscreen items: it must be idempotent, leave copper and
footprints alone, keep its ASCII glyphs on a fixed grid, stay off every pad and stay inside
the board outline."""
import shutil
import tomllib
from pathlib import Path

import pcbnew
import pytest

from hw.tools import artwork

CARRIER = Path(__file__).resolve().parents[1] / "carrier"
BOARD = CARRIER / "carrier.kicad_pcb"
ART = CARRIER / "artwork.toml"
MM = pcbnew.FromMM
PAD_MARGIN_MM = 0.2          # the test's own bar; the design keeps 0.3 mm


def _inflate(bb, m: int) -> pcbnew.BOX2I:
    """`bb` grown by m nm on every side (BOX2I.Inflate's Python overload misbehaves)."""
    return pcbnew.BOX2I(pcbnew.VECTOR2I(bb.GetLeft() - m, bb.GetTop() - m),
                        pcbnew.VECTOR2I(bb.GetWidth() + 2 * m, bb.GetHeight() + 2 * m))


def _load(tmp_path: Path) -> pcbnew.BOARD:
    dst = tmp_path / BOARD.name
    shutil.copy(BOARD, dst)
    board = pcbnew.LoadBoard(str(dst))
    artwork.remove_art(board)      # the committed board already carries the art
    assert not artwork.art_groups(board)
    return board


def _group(board) -> pcbnew.PCB_GROUP:
    gs = artwork.art_groups(board)
    assert len(gs) == 1, f"{len(gs)} groups named {artwork.GROUP}"
    return gs[0]


def _items(board) -> list:
    return artwork.group_items(_group(board))


def _counts(board) -> tuple:
    return (len(board.GetFootprints()), len(board.GetTracks()), len(board.Zones()),
            len(board.GetDrawings()))


@pytest.fixture(scope="module")
def spec() -> dict:
    return tomllib.loads(ART.read_text())


@pytest.fixture(scope="module")
def applied(tmp_path_factory) -> pcbnew.BOARD:
    board = _load(tmp_path_factory.mktemp("art"))
    artwork.apply(board, ART)
    return board


def test_idempotent(tmp_path):
    board = _load(tmp_path)
    before = _counts(board)
    artwork.apply(board, ART)
    once = _counts(board)
    n_items = len(_items(board))
    artwork.apply(board, ART)
    assert _counts(board) == once
    assert len(_items(board)) == n_items
    assert once[3] - before[3] == n_items, "every drawing the art adds is in the group"
    assert len(artwork.art_groups(board)) == 1


def test_touches_no_copper_or_footprints(tmp_path):
    board = _load(tmp_path)
    before = _counts(board)
    pads = sorted((p.GetParentFootprint().GetReference(), p.GetNumber(), p.GetPosition().x,
                   p.GetPosition().y, p.GetNetname()) for p in board.GetPads())
    artwork.apply(board, ART)
    after = _counts(board)
    assert after[:3] == before[:3]
    assert sorted((p.GetParentFootprint().GetReference(), p.GetNumber(), p.GetPosition().x,
                   p.GetPosition().y, p.GetNetname()) for p in board.GetPads()) == pads


def test_all_items_on_front_silkscreen(applied):
    for it in _items(applied):
        assert it.GetLayer() == pcbnew.F_SilkS, it.GetClass()


def test_name_text(applied, spec):
    n = spec["name"]
    texts = [it for it in _items(applied) if it.GetClass() == "PCB_TEXT"
             and it.GetText() == n["text"]]
    assert len(texts) == 1
    t = texts[0]
    assert n["text"] == "a2nix carrier rev 0"
    assert t.GetTextHeight() == MM(n["height_mm"]) and t.GetTextThickness() == MM(n["thickness_mm"])


def test_logo_present(applied):
    shapes = [it for it in _items(applied) if it.GetClass() == "PCB_SHAPE"]
    assert shapes, "the open-source-hardware logo is graphic items in the group"
    assert not [fp for fp in applied.GetFootprints() if "OSHW" in fp.GetFPIDAsString()]


def test_ascii_glyphs_on_a_fixed_grid(applied, spec):
    grid = spec["grid"]
    cp, rp = MM(grid["col_pitch_mm"]), MM(grid["row_pitch_mm"])
    glyphs = {}
    for it in _items(applied):
        if it.GetClass() == "PCB_TEXT" and len(it.GetText()) == 1:
            glyphs.setdefault(it.GetTextHeight(), []).append(it)
    assert set(glyphs) == {MM(grid["height_mm"])}
    for blk in spec["ascii"]:
        x0, y0 = MM(blk["x_mm"]), MM(blk["y_mm"])
        lines = blk["art"].strip("\n").split("\n")
        want = {(x0 + c * cp, y0 + r * rp, ch)
                for r, line in enumerate(lines) for c, ch in enumerate(line) if ch != " "}
        w = max(len(ln) for ln in lines)
        h = len(lines)
        have = {(it.GetX(), it.GetY(), it.GetText()) for it in glyphs[MM(grid["height_mm"])]
                if x0 - cp // 2 <= it.GetX() <= x0 + w * cp and y0 - rp // 2 <= it.GetY() <= y0 + h * rp}
        # KiCad stores nm integers; allow 1 um for rounding
        for gx, gy, ch in want:
            assert any(abs(gx - hx) <= 1000 and abs(gy - hy) <= 1000 and ch == hc
                       for hx, hy, hc in have), f"missing {ch!r} at {gx / 1e6:.3f},{gy / 1e6:.3f}"
        assert len(have) == len(want), blk.get("name")
    total = sum(len(v) for v in glyphs.values())
    assert total == sum(1 for b in spec["ascii"] for ch in b["art"] if ch not in " \n")


def test_no_art_touches_a_pad(applied):
    pads = [(p.GetParentFootprint().GetReference(), p.GetNumber(),
             _inflate(p.GetBoundingBox(), MM(PAD_MARGIN_MM))) for p in applied.GetPads()]
    bad = []
    for it in _items(applied):
        bb = it.GetBoundingBox()
        for ref, num, pb in pads:
            if pb.Intersects(bb):
                bad.append((it.GetClass(), getattr(it, "GetText", lambda: "")(), ref, num))
    assert not bad, bad[:10]


def test_all_art_inside_the_outline(applied):
    outline = pcbnew.SHAPE_POLY_SET()
    assert applied.GetBoardPolygonOutlines(outline, True)
    for it in _items(applied):
        bb = it.GetBoundingBox()
        for x, y in ((bb.GetLeft(), bb.GetTop()), (bb.GetRight(), bb.GetTop()),
                     (bb.GetLeft(), bb.GetBottom()), (bb.GetRight(), bb.GetBottom())):
            assert outline.Contains(pcbnew.VECTOR2I(x, y)), (it.GetClass(), x / 1e6, y / 1e6)
        # and clear of the finger-bevel band, along the board's bottom edge
        assert bb.GetBottom() < MM(68.0 - 1.0)
