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
    layers/<layer>.svg                   F.Cu, B.Cu, F.Silkscreen, Edge.Cuts
    schematic.pdf

Everything runs on a temporary copy of the board, its project file and the schematic:
kicad-cli and KiKit rewrite the .kicad_pro and write a .kicad_prl (ruling R21).
"""
import argparse
import csv
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import pcbnew

LAYERS = ["F.Cu", "B.Cu", "F.Silkscreen", "Edge.Cuts"]


def run(cmd: list[str], cwd: Path | None = None) -> None:
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
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


def build(board_src: Path, sch_src: Path, out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    (out / "layers").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        board, sch = tmp / board_src.name, tmp / sch_src.name
        shutil.copy(board_src, board)
        shutil.copy(sch_src, sch)
        pro = board_src.with_suffix(".kicad_pro")
        if pro.exists():
            shutil.copy(pro, tmp / pro.name)

        # gerbers, BOM, CPL
        kk = tmp / "kikit"
        run(["kikit", "fab", "jlcpcb", "--assembly", "--schematic", str(sch),
             "--field", "LCSC", str(board), str(kk)])
        shutil.copy(kk / "gerbers.zip", out / "gerbers.zip")
        shutil.copy(kk / "bom.csv", out / "bom.csv")
        write_cpl(kk / "pos.csv", out / "cpl.csv", not_placed(pcbnew.LoadBoard(str(board))))

        # interactive BOM (needs a display for its wx import)
        run(["xvfb-run", "-a", "generate_interactive_bom", "--no-browser",
             "--dest-dir", str(tmp / "ibom"), "--name-format", "ibom",
             "--include-tracks", str(board)])
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
