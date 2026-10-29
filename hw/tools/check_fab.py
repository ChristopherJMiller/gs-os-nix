# SPDX-License-Identifier: GPL-3.0-or-later
"""hw-fab: the JLCPCB order package is complete.

    python3 hw/tools/check_fab.py --board BOARD.kicad_pcb DIR                    # HW FAB OK
    python3 hw/tools/check_fab.py --selftest --board BOARD.kicad_pcb --sch SCH   # HW FAB SELFTEST OK

DIR is what `hw/tools/fab.py` wrote. It must hold:

* gerbers.zip with the F_Cu, B_Cu, F_Mask, B_Mask, F_Silkscreen, B_Silkscreen and
  Edge_Cuts gerbers and a drill file;
* cpl.csv with exactly one row for every board reference that is neither
  `Assembly = "hand"` nor board copper (`Fab = "pcb"`), and no row for any other;
* bom.csv, both renders, ibom.html, schematic.pdf and layers/*.svg, non-empty.

The selftest builds the package from a temporary copy of the board with Edge.Cuts
deleted. KiKit may refuse it, or the zip may lose its outline gerber; either way the
package must not pass.
"""
import argparse
import csv
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

import pcbnew

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

# KiKit names the gerbers by layer role; the KiCad plot names are accepted too.
GERBERS = {"F_Cu": "CuTop", "B_Cu": "CuBottom", "F_Mask": "MaskTop", "B_Mask": "MaskBottom",
           "F_Silkscreen": "SilkTop", "B_Silkscreen": "SilkBottom", "Edge_Cuts": "EdgeCuts"}
FILES = ["bom.csv", "cpl.csv", "ibom.html", "render-top.png", "render-bottom.png",
         "schematic.pdf", "layers/F.Cu.svg", "layers/B.Cu.svg",
         "layers/F.Silkscreen.svg", "layers/Edge.Cuts.svg"]


def placed_refs(board) -> set[str]:
    """References that belong in the CPL: machine-placed parts."""
    out = set()
    for fp in board.GetFootprints():
        f = {x.GetName(): x.GetText() for x in fp.GetFields()}
        if f.get("Assembly") != "hand" and f.get("Fab") != "pcb":
            out.add(fp.GetReference())
    return out


def problems(d: Path, board) -> list[str]:
    out = []
    z = d / "gerbers.zip"
    if not z.is_file():
        out.append("gerbers.zip is missing")
    else:
        names = zipfile.ZipFile(z).namelist()
        for g, alt in GERBERS.items():
            if not any(g in n or alt in n for n in names):
                out.append(f"gerbers.zip has no {g} gerber")
        if not any(n.lower().endswith(".drl") for n in names):
            out.append("gerbers.zip has no drill file")
    for f in FILES:
        p = d / f
        if not p.is_file() or p.stat().st_size == 0:
            out.append(f"{f} is missing or empty")
    cpl = d / "cpl.csv"
    if cpl.is_file():
        with cpl.open() as fh:
            rows = [r["Designator"] for r in csv.DictReader(fh)]
        want = placed_refs(board)
        for r in sorted(want):
            if rows.count(r) != 1:
                out.append(f"cpl.csv has {rows.count(r)} rows for {r}, want exactly 1")
        for r in sorted(set(rows) - want):
            out.append(f"cpl.csv has a row for {r}, which is hand-fitted or board copper")
    return out


def selftest(board_path: Path, sch: Path) -> int:
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        board = pcbnew.LoadBoard(str(board_path))
        for dr in list(board.GetDrawings()):
            if dr.GetLayer() == pcbnew.Edge_Cuts:
                board.RemoveNative(dr)
        copy = tmp / "carrier.kicad_pcb"
        pcbnew.SaveBoard(str(copy), board)
        fab = Path(__file__).with_name("fab.py")
        if not fab.is_file():
            print(f"selftest: {fab} does not exist")
            return 1
        r = subprocess.run([sys.executable, str(fab), "--board", str(copy), "--sch", str(sch),
                            "--out", str(tmp / "out")], capture_output=True, text=True)
        if r.returncode == 0:
            found = problems(tmp / "out", board)
            if not any("Edge_Cuts" in p for p in found):
                print("selftest: a package built without Edge.Cuts was not found incomplete "
                      f"(got {found[:3]})")
                return 1
        else:
            print("selftest: fab.py refused the board without Edge.Cuts: "
                  f"{(r.stderr.strip().splitlines() or ['(no message)'])[-1]}")
    print("HW FAB SELFTEST OK")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dir", type=Path, nargs="?")
    ap.add_argument("--board", type=Path, required=True)
    ap.add_argument("--sch", type=Path)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        if not a.sch:
            ap.error("--selftest needs --sch")
        return selftest(a.board, a.sch)
    if not a.dir:
        ap.error("DIR is required")
    found = problems(a.dir, pcbnew.LoadBoard(str(a.board)))
    for p in found:
        print(f"HW FAB: {p}")
    if found:
        print(f"HW FAB: {len(found)} problems")
        return 1
    print("HW FAB OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
