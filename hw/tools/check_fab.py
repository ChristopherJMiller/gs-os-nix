# SPDX-License-Identifier: GPL-3.0-or-later
"""hw-fab: the JLCPCB order package is complete.

    python3 hw/tools/check_fab.py --board BOARD.kicad_pcb DIR                    # HW FAB OK
    python3 hw/tools/check_fab.py --selftest --board BOARD.kicad_pcb DIR         # HW FAB SELFTEST OK

DIR is what `hw/tools/fab.py` wrote. It must hold:

* gerbers.zip with the F_Cu, B_Cu, F_Mask, B_Mask, F_Silkscreen, B_Silkscreen and
  Edge_Cuts gerbers and a drill file;
* cpl.csv with exactly one row for every board reference that is neither
  `Assembly = "hand"` nor board copper (`Fab = "pcb"`), and no row for any other;
* bom.csv, both renders, ibom.html, schematic.pdf and layers/*.svg, non-empty.

The selftest plants faults in copies of a built package (a gerber and the drill removed
from the zip, CPL rows removed, doubled or added for J1 and a hand part, a render emptied
or removed); each must be found with its own message, and the unmutated package must pass.
"""
import argparse
import csv
import shutil
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


def _copy(pkg: Path, dst: Path) -> Path:
    shutil.copytree(pkg, dst)
    return dst


def _zip_drop(d: Path, keep) -> None:
    z = d / "gerbers.zip"
    with zipfile.ZipFile(z) as zin:
        items = [(i, zin.read(i)) for i in zin.infolist() if keep(i.filename)]
    with zipfile.ZipFile(z, "w") as zout:
        for i, data in items:
            zout.writestr(i, data)


def _cpl_edit(d: Path, edit) -> None:
    p = d / "cpl.csv"
    with p.open() as fh:
        rd = csv.DictReader(fh)
        names, rows = rd.fieldnames, list(rd)
    rows = edit(rows)
    with p.open("w", newline="") as fh:
        w = csv.DictWriter(fh, names, lineterminator="\n")
        w.writeheader()
        w.writerows(rows)


def _hand_ref(board) -> str:
    return next(r for r in sorted(f.GetReference() for f in board.GetFootprints())
                if r not in placed_refs(board) and r != "J1")


MUTATIONS = {
    "Edge_Cuts gerber removed": (
        lambda d, b: _zip_drop(d, lambda n: "EdgeCuts" not in n and "Edge_Cuts" not in n),
        "no Edge_Cuts gerber"),
    "drill files removed": (
        lambda d, b: _zip_drop(d, lambda n: not n.lower().endswith(".drl")),
        "no drill file"),
    "gerbers.zip missing": (lambda d, b: (d / "gerbers.zip").unlink(), "gerbers.zip is missing"),
    "CPL row for R1 removed": (
        lambda d, b: _cpl_edit(d, lambda rows: [r for r in rows if r["Designator"] != "R1"]),
        "0 rows for R1"),
    "CPL row for R1 doubled": (
        lambda d, b: _cpl_edit(d, lambda rows: rows + [r for r in rows
                                                       if r["Designator"] == "R1"]),
        "2 rows for R1"),
    "CPL row for J1 added": (
        lambda d, b: _cpl_edit(d, lambda rows: rows + [{**rows[0], "Designator": "J1"}]),
        "row for J1, which is hand-fitted"),
    "CPL row for a hand part added": (
        lambda d, b: _cpl_edit(d, lambda rows: rows + [{**rows[0],
                                                        "Designator": _hand_ref(b)}]),
        "which is hand-fitted or board copper"),
    "render-top.png emptied": (lambda d, b: (d / "render-top.png").write_bytes(b""),
                               "render-top.png is missing or empty"),
    "render-bottom.png removed": (lambda d, b: (d / "render-bottom.png").unlink(),
                                  "render-bottom.png is missing or empty"),
}


def selftest(board_path: Path, pkg: Path) -> int:
    """Plant each fault in a copy of the built package; each must be found as its own message."""
    board = pcbnew.LoadBoard(str(board_path))
    bad = 0
    base = problems(pkg, board)
    if base:
        print(f"selftest: the unmutated package already fails: {base[:3]}")
        bad += 1
    with tempfile.TemporaryDirectory() as tmp:
        for i, (name, (mutate, expect)) in enumerate(MUTATIONS.items()):
            d = _copy(pkg, Path(tmp) / f"m{i}")
            for p in [d, *d.rglob("*")]:
                p.chmod(p.stat().st_mode | 0o200)
            mutate(d, board)
            found = problems(d, board)
            if not any(expect in p for p in found):
                print(f"selftest: planted fault '{name}' was not found as {expect!r} "
                      f"(got {found[:3]})")
                bad += 1
    if bad:
        return 1
    print("HW FAB SELFTEST OK")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dir", type=Path)
    ap.add_argument("--board", type=Path, required=True)
    ap.add_argument("--selftest", action="store_true",
                    help="plant each fault in a copy of the built package DIR")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest(a.board, a.dir)
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
