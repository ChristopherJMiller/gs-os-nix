# SPDX-License-Identifier: GPL-3.0-or-later
"""fab: the JLCPCB order package and the review artefacts for the routed carrier.

    python3 -m hw.tools.fab --board B.kicad_pcb --sch S.kicad_sch --out DIR

DIR gets:

    gerbers.zip      gerbers and drill, from `kikit fab jlcpcb --assembly --field LCSC`
    bom.csv          JLCPCB's BOM (Comment, Designator, Footprint, LCSC), hand parts included
    cpl.csv          JLCPCB's CPL, KiKit's rotation corrections applied; `Assembly = "hand"`
                     parts and board copper (`Fab = "pcb"`, the edge fingers) left out
    ibom.html        the Interactive HTML BOM
    render-top.png, render-bottom.png    board-only 3D renders
    order-notes.txt  what to tell JLCPCB: 4 layers, JLC04161H-7628, ENIG, gold fingers
    layers/<layer>.svg                   F.Cu, In1.Cu, In2.Cu, B.Cu, F.Silkscreen, Edge.Cuts
    schematic.pdf

Everything runs on a temporary copy of the board, its project file, its custom design rules
(.kicad_dru: KiKit's DRC needs the Clock class's exemptions) and the schematic: kicad-cli and
KiKit rewrite the .kicad_pro and write a .kicad_prl (ruling R21).
"""
import argparse
import csv
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import pcbnew

LAYERS = ["F.Cu", "In1.Cu", "In2.Cu", "B.Cu", "F.Silkscreen", "Edge.Cuts"]


def order_notes(board) -> str:
    """The order options for JLCPCB's form, from the board itself (its size and the J1
    finger tab); the stackup is build_board.STACKUP's."""
    xs = [pcbnew.ToMM(p.x) for d in board.GetDrawings() if d.GetLayer() == pcbnew.Edge_Cuts
          and hasattr(d, "GetStart") for p in (d.GetStart(), d.GetEnd())]
    ys = [pcbnew.ToMM(p.y) for d in board.GetDrawings() if d.GetLayer() == pcbnew.Edge_Cuts
          and hasattr(d, "GetStart") for p in (d.GetStart(), d.GetEnd())]
    size = f"{max(xs) - min(xs):.1f} x {max(ys) - min(ys):.1f} mm"
    return (f"JLCPCB order options for the carrier ({size}):\n"
            "  Layers: 4 layers\n"
            "  PCB thickness: 1.6 mm (the slot takes 1.57 mm)\n"
            "  Layer stack-up: JLC04161H-7628 (7628 prepreg 0.2104 mm outside the 1.065 mm "
            "core), impedance not controlled\n"
            "  Copper weight: 1 oz outer, 0.5 oz inner\n"
            "  Surface finish: ENIG (immersion gold)\n"
            "  Edge connector: gold fingers (bevelled), 45 degree bevel on the J1 tab (the bevel keepout is "
            "1 mm); no inner copper in the tab\n"
            "  Vias: through only; tented\n"
            "  Order number: Remove order number (JLCPCB offers the option; the card "
            "has no place kept for it on the silkscreen)\n"
            "  Assembly: top side, Standard PCBA\n")


def run(cmd: list[str], cwd: Path | None = None, env: dict[str, str] | None = None) -> None:
    """Run `cmd`, with `env` added to the environment; exit with its output if it fails."""
    full_env = {**os.environ, **env} if env else None
    r = subprocess.run(cmd, cwd=cwd, env=full_env, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"fab: {' '.join(cmd[:3])} failed ({r.returncode}):\n{r.stdout}{r.stderr}")


def not_placed(board) -> set[str]:
    """References that stay out of the CPL: hand-fitted parts and board copper."""
    out = set()
    for fp in board.GetFootprints():
        f = {x.GetName(): x.GetText() for x in fp.GetFields()}
        if f.get("Assembly") == "hand" or f.get("Fab") == "pcb":
            out.add(fp.GetReference())
    return out


def write_cpl(pos: Path, dest: Path, skip: set[str]) -> int:
    with pos.open() as fh:
        rd = csv.DictReader(fh)
        rows = [r for r in rd if r["Designator"] not in skip]
        names = rd.fieldnames
    with dest.open("w", newline="") as fh:
        w = csv.DictWriter(fh, names, lineterminator="\n")
        w.writeheader()
        w.writerows(rows)
    return len(rows)


# The iBOM's own headless switch: it never creates a wx.App, so it needs no X server. Under
# xvfb-run it failed now and then on a loaded build machine (wx found no display and GTK
# segfaulted), with both `xvfb-run -a` and `-d`.
IBOM_ENV = {"INTERACTIVE_HTML_BOM_NO_DISPLAY": "1"}


def ibom_command(board: Path, dest: Path) -> list[str]:
    """The Interactive HTML BOM as `ibom.html` in `dest`. Run it with IBOM_ENV."""
    return ["generate_interactive_bom", "--no-browser",
            "--dest-dir", str(dest), "--name-format", "ibom",
            "--include-tracks", str(board)]


def build(board_src: Path, sch_src: Path, out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    (out / "layers").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        board, sch = tmp / board_src.name, tmp / sch_src.name
        shutil.copy(board_src, board)
        shutil.copy(sch_src, sch)
        for suffix in (".kicad_pro", ".kicad_dru"):    # the custom rules too: KiKit runs the DRC
            src = board_src.with_suffix(suffix)
            if src.exists():
                shutil.copy(src, board.with_suffix(suffix))

        # gerbers, BOM, CPL
        kk = tmp / "kikit"
        run(["kikit", "fab", "jlcpcb", "--assembly", "--schematic", str(sch),
             "--field", "LCSC", str(board), str(kk)])
        shutil.copy(kk / "gerbers.zip", out / "gerbers.zip")
        shutil.copy(kk / "bom.csv", out / "bom.csv")
        write_cpl(kk / "pos.csv", out / "cpl.csv", not_placed(pcbnew.LoadBoard(str(board))))

        (out / "order-notes.txt").write_text(order_notes(pcbnew.LoadBoard(str(board))))

        # interactive BOM, headless
        run(ibom_command(board, tmp / "ibom"), env=IBOM_ENV)
        shutil.copy(tmp / "ibom" / "ibom.html", out / "ibom.html")

        # renders, board only
        for side in ("top", "bottom"):
            run(["kicad-cli", "pcb", "render", "--side", side, "--width", "1400",
                 "--height", "1300", "--quality", "basic", "--background", "opaque",
                 "--output", str(out / f"render-{side}.png"), str(board)])

        # per-layer SVGs, cropped to the board
        for layer in LAYERS:
            run(["kicad-cli", "pcb", "export", "svg", "--layers", layer, "--mode-single",
                 "--page-size-mode", "2", "--exclude-drawing-sheet",
                 "--output", str(out / "layers" / f"{layer}.svg"), str(board)])

        run(["kicad-cli", "sch", "export", "pdf", "--output", str(out / "schematic.pdf"),
             str(sch)])


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--board", type=Path, required=True)
    ap.add_argument("--sch", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args(argv)
    build(a.board, a.sch, a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
