# SPDX-License-Identifier: GPL-3.0-or-later
"""hw-bom: every part that is ordered has an LCSC number.

    python3 hw/tools/check_bom.py --netlist NET BOARD.kicad_pcb              # HW BOM OK
    python3 hw/tools/check_bom.py --selftest --netlist NET BOARD.kicad_pcb   # HW BOM SELFTEST OK

Every footprint on the board and every part in the netlist must carry an `LCSC` field
matching ^C\\d+$, except board copper (ruling R8): the edge fingers J1 carry `Fab = "pcb"`
and no LCSC. The exempt set must be exactly {J1}, so a part cannot dodge the gate by
gaining `Fab = "pcb"`. Parts with `Assembly = "hand"` still need ordering and are checked.

The selftest plants, on temporary copies: an LCSC deleted, an LCSC of `12345`, a second
part marked `Fab = "pcb"`, and (in the netlist) an LCSC of `12345`; each must fail.
"""
import argparse
import re
import sys
import tempfile
from pathlib import Path

import pcbnew

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from hw.tools.netlist import Netlist, PartInfo, load

LCSC = re.compile(r"^C\d+$")
EXEMPT = {"J1"}


def _fields(fp) -> dict[str, str]:
    return {f.GetName(): f.GetText() for f in fp.GetFields()}


def _check(where: str, parts: dict[str, dict[str, str]]) -> list[str]:
    out = []
    exempt = {r for r, f in parts.items() if f.get("Fab") == "pcb"}
    if exempt != EXEMPT:
        out.append(f"{where}: parts exempt from the BOM (Fab = pcb) are {sorted(exempt)}, "
                   f"must be exactly {sorted(EXEMPT)}")
    for ref in sorted(parts):
        if ref in exempt & EXEMPT:
            continue
        v = parts[ref].get("LCSC", "")
        if not LCSC.match(v):
            out.append(f"{where}: {ref} LCSC is {v!r}, want C<digits>")
    return out


def problems_board(board) -> list[str]:
    return _check("board", {fp.GetReference(): _fields(fp) for fp in board.GetFootprints()})


def problems_netlist(nl: Netlist) -> list[str]:
    return _check("netlist", {r: p.fields for r, p in nl.parts.items()})


def problems(board_path: Path, nl: Netlist) -> list[str]:
    return problems_board(pcbnew.LoadBoard(str(board_path))) + problems_netlist(nl)


def _c1(board):
    return next(fp for fp in board.GetFootprints() if fp.GetReference() == "C1")


def _delete(board):
    for f in _c1(board).GetFields():
        if f.GetName() == "LCSC":
            f.SetText("")


def _garbage(board):
    for f in _c1(board).GetFields():
        if f.GetName() == "LCSC":
            f.SetText("12345")


def _second_pcb(board):
    """C1 becomes 'board copper' too: it has an LCSC, so only the exempt-set rule sees it."""
    fp = _c1(board)
    fp.SetField("Fab", "pcb")


BOARD_MUTATIONS = {
    "LCSC deleted": (_delete, "C1 LCSC is ''"),
    "LCSC 12345": (_garbage, "C1 LCSC is '12345'"),
    "second part Fab = pcb": (_second_pcb, "must be exactly ['J1']"),
}


def selftest(board_path: Path, nl: Netlist) -> int:
    bad = 0
    if problems(board_path, nl):
        print(f"selftest: the unmutated inputs already fail: {problems(board_path, nl)[:3]}")
        bad += 1
    with tempfile.TemporaryDirectory() as tmp:
        for i, (name, (mutate, expect)) in enumerate(BOARD_MUTATIONS.items()):
            copy = Path(tmp) / f"m{i}.kicad_pcb"
            board = pcbnew.LoadBoard(str(board_path))
            mutate(board)
            pcbnew.SaveBoard(str(copy), board)
            found = problems_board(pcbnew.LoadBoard(str(copy)))
            if not any(expect in p for p in found):
                print(f"selftest: planted fault '{name}' was not found as {expect!r} "
                      f"(got {found[:3]})")
                bad += 1
    c1 = nl.parts["C1"]
    parts = dict(nl.parts)
    parts["C1"] = PartInfo(c1.value, c1.footprint, {**c1.fields, "LCSC": "12345"})
    if not any("C1 LCSC is '12345'" in p for p in problems_netlist(Netlist(parts, nl.nets))):
        print("selftest: planted netlist LCSC 12345 was not found")
        bad += 1
    if bad:
        return 1
    print("HW BOM SELFTEST OK")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("board", type=Path)
    ap.add_argument("--netlist", type=Path, required=True)
    ap.add_argument("--selftest", action="store_true",
                    help="plant each fault in a copy; every one must be found")
    a = ap.parse_args(argv)
    nl = load(a.netlist)
    if a.selftest:
        return selftest(a.board, nl)
    found = problems(a.board, nl)
    for p in found:
        print(f"HW BOM: {p}")
    if found:
        print(f"HW BOM: {len(found)} problems")
        return 1
    print("HW BOM OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
