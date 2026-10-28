# SPDX-License-Identifier: GPL-3.0-or-later
"""Footprints KiCad's library lacks, generated as KiCad 10 .kicad_mod text.

    python3 -m hw.tools.footprints [DIR]      # default: hw/lib/gsnix.pretty

The output is committed: it is a design file, so each footprint's descr carries
CERN-OHL-S-2.0, and hw/tests/test_footprints.py checks that regenerating it is
byte-identical. Nothing here is random: each UUID is a uuid5 of the footprint
name and the item's place in it (KiCad assigns fresh ones when it loads a
footprint, so these only keep the file stable). Dimensions and their sources
live in slotgeom.
"""
import sys
import uuid
from pathlib import Path

from hw.tools import slotgeom as g

LIB = "gsnix"
EDGE_ID = f"{LIB}:AppleII_Slot_Edge"
SODIMM_ID = f"{LIB}:SODIMM200_DDR2"
VERSION = 20260206      # the footprint format KiCad 10.0 writes (its own library's token)
LICENSE = "SPDX-License-Identifier: CERN-OHL-S-2.0"
_NS = uuid.uuid5(uuid.NAMESPACE_URL, "gs-os-nix/hw/lib/gsnix.pretty")


def _q(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _num(v: float) -> str:
    s = f"{round(v, 6):.6f}".rstrip("0").rstrip(".")
    return "0" if s == "-0" else s


def _sexpr(node: tuple, depth: int = 0) -> list[str]:
    """KiCad's layout: atoms on the opening line, each sub-list on its own."""
    ind = "\t" * depth
    atoms = " ".join(_num(a) if isinstance(a, (int, float)) else a
                     for a in node if not isinstance(a, tuple))
    subs = [a for a in node if isinstance(a, tuple)]
    if not subs:
        return [f"{ind}({atoms})"]
    out = [f"{ind}({atoms}"]
    for s in subs:
        out += _sexpr(s, depth + 1)
    return out + [f"{ind})"]


def _stroke(width: float, dashed: bool = False) -> tuple:
    return ("stroke", ("width", width), ("type", "dash" if dashed else "solid"))


class _Footprint:
    def __init__(self, name: str, descr: str, tags: str, attr: tuple[str, ...]):
        self.name, self.descr, self.tags, self.attr = name, descr, tags, attr
        self.props: list[tuple] = []
        self.items: list[tuple] = []
        self.pads: list[tuple] = []
        self._n = 0

    def _uuid(self, kind: str) -> tuple:
        self._n += 1
        return ("uuid", _q(str(uuid.uuid5(_NS, f"{self.name}/{kind}/{self._n}"))))

    def prop(self, key: str, value: str, at: tuple[float, float], layer: str,
             hidden: bool = False, size: float = 1.0) -> None:
        extra = (("unlocked", "yes"),) if hidden else ()
        hide = (("hide", "yes"),) if hidden else ()
        self.props.append(("property", _q(key), _q(value), ("at", *at, 0), *extra,
                           ("layer", _q(layer)), *hide, self._uuid("property"),
                           ("effects", ("font", ("size", size, size), ("thickness", 0.15)))))

    def line(self, a, b, layer: str, width: float, dashed: bool = False) -> None:
        self.items.append(("fp_line", ("start", *a), ("end", *b),
                           _stroke(width, dashed), ("layer", _q(layer)),
                           self._uuid("fp_line")))

    def rect(self, a, b, layer: str, width: float) -> None:
        self.items.append(("fp_rect", ("start", *a), ("end", *b),
                           _stroke(width), ("fill", "no"),
                           ("layer", _q(layer)), self._uuid("fp_rect")))

    def dot(self, c, r: float, layer: str) -> None:
        self.items.append(("fp_circle", ("center", *c), ("end", c[0] + r, c[1]),
                           _stroke(0.12), ("fill", "yes"),
                           ("layer", _q(layer)), self._uuid("fp_circle")))

    def smd(self, num: str, at, size, layers: tuple[str, ...]) -> None:
        self.pads.append(("pad", _q(num), "smd", "rect", ("at", *at), ("size", *size),
                          ("layers", *map(_q, layers)), self._uuid("pad")))

    def npth(self, at, drill: float) -> None:
        self.pads.append(("pad", _q(""), "np_thru_hole", "circle", ("at", *at),
                          ("size", drill, drill), ("drill", drill),
                          ("layers", _q("*.Cu"), _q("*.Mask")), self._uuid("pad")))

    def text(self) -> str:
        node = ("footprint", _q(self.name), ("version", VERSION),
                ("generator", _q("gsnix-footprints")), ("layer", _q("F.Cu")),
                ("descr", _q(self.descr)), ("tags", _q(self.tags)), *self.props,
                ("attr", *self.attr), ("duplicate_pad_numbers_are_jumpers", "no"),
                *self.items, *self.pads, ("embedded_fonts", "no"))
        return "\n".join(_sexpr(node)) + "\n"


def edge_fingers() -> str:
    """The Apple II slot's 2 x 25 gold fingers.

    Frame: the origin is the middle of the tab's bottom edge (the board edge),
    x grows toward the card's rear, and the fingers run up into the board (-y).
    Pins 1-25 are on F.Cu, front to rear; pin 51 - n is behind pin n on B.Cu.
    Pads are SMD copper and mask only: gold fingers take no paste.
    """
    w, d = g.TAB_WIDTH_MM, g.TAB_DEPTH_MM
    fp = _Footprint(
        EDGE_ID.split(":")[1],
        "Apple II peripheral slot edge fingers, 2 x 25 at 2.54 mm: pins 1-25 on "
        "F.Cu from the front, pin 51-n behind pin n on B.Cu; origin at the middle "
        "of the tab's bottom edge. Generated by hw/tools/footprints.py. " + LICENSE,
        "Apple II slot edge connector gold fingers",
        ("exclude_from_pos_files", "exclude_from_bom"))
    fp.prop("Reference", "REF**", (0, -d - 1.5), "F.Fab")
    fp.prop("Value", "AppleII_Slot_Edge", (0, -d - 3), "F.Fab")
    fp.prop("Datasheet", "", (0, 0), "F.Fab", hidden=True, size=1.27)
    fp.prop("Description", "", (0, 0), "F.Fab", hidden=True, size=1.27)
    for side in ("F", "B"):
        fp.rect((-w / 2, -d), (w / 2, 0), f"{side}.CrtYd", 0.05)
    fp.rect((-w / 2, -d), (w / 2, 0), "F.Fab", 0.1)
    fp.line((-w / 2, -g.BEVEL_KEEPOUT_MM), (w / 2, -g.BEVEL_KEEPOUT_MM), "F.Fab", 0.1,
            dashed=True)
    y = -(g.BEVEL_KEEPOUT_MM + g.FINGER_L_MM / 2)
    size = (g.FINGER_W_MM, g.FINGER_L_MM)
    for n in range(1, 26):
        x = (n - 13) * g.PITCH_MM         # pin 13 is the middle of 25
        fp.smd(str(n), (x, y), size, ("F.Cu", "F.Mask"))
        fp.smd(str(51 - n), (x, y), size, ("B.Cu", "B.Mask"))
    fp.pads.sort(key=lambda p: int(p[1].strip('"')))
    return fp.text()


def _sodimm_pin(n: int) -> tuple[float, float]:
    """Pad n's centre: the origin is midway between the bosses, pin 1 toward -x,
    odd row at +y, even (module) row at -y, as sheet 2 draws it seen from above."""
    k = (n - 1) // 2                     # position within its row, 0..99
    x = (-g.SODIMM_BOSS_PITCH_MM / 2 + g.SODIMM_BOSS1_TO_PIN1_MM
         + (g.SODIMM_ROW_STAGGER_MM if n % 2 == 0 else 0)
         + k * g.SODIMM_PITCH_MM
         + (g.SODIMM_KEY_GAP_MM - g.SODIMM_PITCH_MM if n > 40 else 0))  # 41/42 on: past the key
    y = g.SODIMM_PAD_INNER_MM + g.SODIMM_PAD_L_MM / 2
    return round(x, 4), (y if n % 2 else -y)


def sodimm200() -> str:
    """The DDR2 SO-DIMM 200 socket, TE 1473005-4, from its drawing's sheet 2."""
    fp = _Footprint(
        SODIMM_ID.split(":")[1],
        "DDR2 SO-DIMM 200-pin socket, 0.6 mm pitch, standard type, "
        f"{_num(g.SODIMM_HEIGHT_MM)} mm module height: {g.SODIMM_MPN} (LCSC {g.SODIMM_LCSC}), land pattern from TE drawing "
        "C-1473005 rev J3 sheet 2; origin midway between the bosses, pin 1 at -x, "
        "module toward -y. Generated by hw/tools/footprints.py. " + LICENSE,
        "SODIMM SO-DIMM DIMM DDR2 200 socket",
        ("smd",))
    pad_out = g.SODIMM_PAD_INNER_MM + g.SODIMM_PAD_L_MM
    fp.prop("Reference", "REF**", (0, pad_out + 1.5), "F.SilkS")
    fp.prop("Value", "SODIMM200_DDR2", (0, -g.SODIMM_HOUSING_HALF_MM - 2), "F.Fab")
    fp.prop("Datasheet", g.SODIMM_DATASHEET, (0, 0), "F.Fab", hidden=True, size=1.27)
    fp.prop("Description", "", (0, 0), "F.Fab", hidden=True, size=1.27)
    # Courtyard: the latches swung open, the pads, and the module above the board.
    cx = g.SODIMM_EJECTOR_W_MM / 2 + 0.25
    fp.rect((-cx, -g.SODIMM_MODULE_REACH_MM - 0.5), (cx, pad_out + 0.5), "F.CrtYd", 0.05)
    hx, hy = g.SODIMM_HOUSING_LEN_MM / 2, g.SODIMM_HOUSING_HALF_MM
    fp.rect((-hx, -hy), (hx, hy), "F.Fab", 0.1)
    for sx in (-1, 1):
        fp.line((sx * (hx + 0.1), -hy), (sx * (hx + 0.1), hy), "F.SilkS", 0.12)
    fp.dot((_sodimm_pin(1)[0], pad_out + 0.6), 0.2, "F.SilkS")       # pin 1
    for n in range(1, g.SODIMM_PINS + 1):
        fp.smd(str(n), _sodimm_pin(n), (g.SODIMM_PAD_W_MM, g.SODIMM_PAD_L_MM),
               ("F.Cu", "F.Mask", "F.Paste"))
    peg_x = g.SODIMM_PEG_INNER_MM + g.SODIMM_PEG_ALONG_MM / 2
    for sx in (-1, 1):
        fp.smd("MP", (sx * peg_x, -g.SODIMM_PEG_OFFSET_MM),
               (g.SODIMM_PEG_ALONG_MM, g.SODIMM_PEG_ACROSS_MM), ("F.Cu", "F.Mask", "F.Paste"))
    fp.npth((-g.SODIMM_BOSS_PITCH_MM / 2, 0), g.SODIMM_BOSS1_DRILL_MM)   # boss 1, pin-1 end
    fp.npth((g.SODIMM_BOSS_PITCH_MM / 2, 0), g.SODIMM_BOSS2_DRILL_MM)
    return fp.text()


def write_library(d: Path) -> list[Path]:
    d = Path(d)
    d.mkdir(parents=True, exist_ok=True)
    out = []
    for fid, make in ((EDGE_ID, edge_fingers), (SODIMM_ID, sodimm200)):
        p = d / f"{fid.split(':')[1]}.kicad_mod"
        p.write_bytes(make().encode())
        out.append(p)
    return out


if __name__ == "__main__":
    default = Path(__file__).resolve().parents[1] / "lib" / "gsnix.pretty"
    for p in write_library(Path(sys.argv[1]) if len(sys.argv) > 1 else default):
        print(p)
