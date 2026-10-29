# SPDX-License-Identifier: GPL-3.0-or-later
"""hw-drc: KiCad's DRC on the routed carrier, plus netlist-vs-board parity.

    python3 hw/tools/check_drc.py --netlist NET BOARD.kicad_pcb              # HW DRC OK
    python3 hw/tools/check_drc.py --selftest --netlist NET BOARD.kicad_pcb   # HW DRC SELFTEST OK

Routing output is not reproducible, so the routed board is committed source and this
gate is what ties it back to the netlist. Two halves:

* `kicad-cli pcb drc --severity-error --refill-zones`: no violations, no unconnected
  items. It runs on a temporary copy of the board and its project file, because
  kicad-cli rewrites the .kicad_pro and writes a .kicad_prl. Warnings (silkscreen) do not
  fail the gate; their count is printed.
* parity, our own: the board's footprints are the netlist's parts, each with the
  netlist's `lib:name`, and every pad is on the net the netlist gives its pin. Pads with
  no netlist pin (J2's solder pegs) must have no net. Tracks cannot cheat this, since a
  net is only ever what the pads say; a short shows up as a DRC violation.

The selftest plants a pad on another net, a deleted footprint, a wrong footprint id and
a track across two pads of different nets and a long +5V track thinned (twice), on temporary copies; each must fail with its
own message.
"""
import argparse
import fnmatch
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import pcbnew

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from hw.tools.netlist import Netlist, load

# Power-track width rule. KiCad's DRC does not enforce net-class width as a minimum.
NECK_MAX_MM = 1.5       # a neck is short: a pad escape or a jog, never a run
NECK_MIN_MM = 0.3       # and even a neck stays this wide, for the 3V3/5V rail current
WIDTH_TOL_MM = 0.001    # 1 um: KiCad stores nm, so 0.3004 passes and 0.2989 fails
# GND is exempt: its pours carry the current; the routed GND tracks are only stitching.
WIDTH_EXEMPT = {"GND"}


# --- KiCad's DRC ----------------------------------------------------------------------

def run_drc(board: Path, all_severities: bool = False) -> dict:
    """kicad-cli's DRC report (parsed JSON) for `board`, run on a temporary copy."""
    with tempfile.TemporaryDirectory() as tmp:
        copy = _copy_project(Path(board), Path(tmp))
        out = Path(tmp) / "drc.json"
        subprocess.run(
            ["kicad-cli", "pcb", "drc", "--format", "json", "--refill-zones",
             "--severity-all" if all_severities else "--severity-error",
             "--output", str(out), str(copy)],
            check=True, capture_output=True, text=True)
        return json.loads(out.read_text())


def _copy_project(board_path: Path, dst: Path) -> Path:
    shutil.copy(board_path, dst / board_path.name)
    pro = board_path.with_suffix(".kicad_pro")
    if pro.exists():
        shutil.copy(pro, dst / pro.name)
    return dst / board_path.name


def _where(items: list) -> str:
    return "; ".join(i.get("description", "?") for i in items)


def problems_drc(board: Path) -> list[str]:
    rep = run_drc(board)
    out = []
    for v in rep.get("violations", []):
        out.append(f"DRC {v['type']}: {v['description']} ({_where(v.get('items', []))})")
    for u in rep["unconnected_items"]:
        out.append(f"DRC unconnected: {u['description']} ({_where(u.get('items', []))})")
    return out


def warning_count(board: Path) -> int:
    rep = run_drc(board, all_severities=True)
    return sum(1 for v in rep.get("violations", []) if v.get("severity") == "warning")


# --- parity -----------------------------------------------------------------------------

def problems_parity(board, nl: Netlist) -> list[str]:
    out = []
    fps = {}
    for fp in board.GetFootprints():
        ref = fp.GetReference()
        if ref in fps:
            out.append(f"parity: reference {ref} is on the board twice")
        fps[ref] = fp
    for ref in sorted(set(nl.parts) - set(fps)):
        out.append(f"parity: footprint {ref} is in the netlist but missing from the board")
    for ref in sorted(set(fps) - set(nl.parts)):
        out.append(f"parity: footprint {ref} is on the board but not in the netlist")

    pin_net: dict[str, dict[str, str]] = {}
    for net, nodes in nl.nets.items():
        for ref, pin in nodes:
            pin_net.setdefault(ref, {})[pin] = net

    for ref in sorted(set(nl.parts) & set(fps)):
        fp, info = fps[ref], nl.parts[ref]
        if fp.GetFPIDAsString() != info.footprint:
            out.append(f"parity: {ref} footprint is {fp.GetFPIDAsString()!r}, "
                       f"netlist says {info.footprint!r}")
        want = pin_net.get(ref, {})
        seen = set()
        for pad in fp.Pads():
            num = pad.GetNumber()
            seen.add(num)
            got = pad.GetNetname() if pad.GetNetCode() else ""
            exp = want.get(num, "")
            if got != exp:
                out.append(f"parity: {ref} pad {num or '(unnumbered)'} is on net "
                           f"{got or '(none)'}, netlist says {exp or '(none)'}")
        for pin in sorted(set(want) - seen):
            out.append(f"parity: {ref} pin {pin} is in the netlist but has no pad")
    return out


# --- power-track width --------------------------------------------------------------------

def power_class(pro_path: Path) -> tuple[list[str], float]:
    """(net patterns of the Power net class, its track width in mm), from the .kicad_pro."""
    ns = json.loads(Path(pro_path).read_text())["net_settings"]
    width = next(c["track_width"] for c in ns["classes"] if c["name"] == "Power")
    pats = [x["pattern"] for x in ns["netclass_patterns"] if x["netclass"] == "Power"]
    return pats, width


def problems_width(board, pro_path: Path) -> list[str]:
    """Every Power-class track is at least the class width, except necks.

    A neck is a segment of at most NECK_MAX_MM and at least NECK_MIN_MM wide that either
    has an end on a pad of its net or has both ends joining other copper of its net (a jog
    between full-width tracks). GND is exempt (WIDTH_EXEMPT)."""
    pats, cls_w = power_class(pro_path)
    mm, tol = pcbnew.ToMM, WIDTH_TOL_MM
    nets = {str(n) for n in board.GetNetsByName().keys()
            if any(fnmatch.fnmatchcase(str(n), q) for q in pats)} - WIDTH_EXEMPT
    tracks = [t for t in board.GetTracks() if t.GetNetname() in nets]
    pads = [p for fp in board.GetFootprints() for p in fp.Pads()]

    def on_pad(t, pt):
        acc = t.GetWidth() // 2 + 1000
        return any(p.GetNetCode() == t.GetNetCode() and p.HitTest(pt, acc) for p in pads)

    def joins(t, pt):
        near = lambda q: abs(q.x - pt.x) <= 1000 and abs(q.y - pt.y) <= 1000
        for o in tracks:
            if o is t:
                continue
            if o.GetClass() == "PCB_VIA":
                if near(o.GetPosition()):
                    return True
            elif near(o.GetStart()) or near(o.GetEnd()):
                return True
        return on_pad(t, pt)

    out = []
    for t in tracks:
        if t.GetClass() == "PCB_VIA":
            continue
        w, length = mm(t.GetWidth()), mm(t.GetLength())
        if w >= cls_w - tol:
            continue
        ends = (t.GetStart(), t.GetEnd())
        neck = (length <= NECK_MAX_MM and w >= NECK_MIN_MM - tol and
                (any(on_pad(t, e) for e in ends) or all(joins(t, e) for e in ends)))
        if not neck:
            s0 = t.GetStart()
            out.append(f"power width: {t.GetNetname()} track {w:.3f} mm wide, "
                       f"{length:.2f} mm long at ({mm(s0.x):.2f}, {mm(s0.y):.2f}) on "
                       f"{t.GetLayerName()}; class width {cls_w} mm, only necks "
                       f"(<= {NECK_MAX_MM} mm, >= {NECK_MIN_MM} mm, at a pad or a jog) "
                       f"may be narrower")
    return out


# --- the gate -----------------------------------------------------------------------------

def problems(board_path: Path, nl: Netlist) -> list[str]:
    board = pcbnew.LoadBoard(str(board_path))
    return (problems_parity(board, nl)
            + problems_width(board, board_path.with_suffix(".kicad_pro"))
            + problems_drc(board_path))


def _pad_move(board):
    """Move a resistor pad to a net its part is not on."""
    fp = next(f for f in board.GetFootprints() if f.GetReference().startswith("R"))
    pads = list(fp.Pads())
    other = next(n for n in board.GetNetsByName().values()
                 if n.GetNetCode() not in (0, pads[0].GetNetCode(), pads[1].GetNetCode()))
    pads[0].SetNet(other)


def _fp_delete(board):
    fp = next(f for f in board.GetFootprints() if f.GetReference() == "C1")
    board.RemoveNative(fp)      # Remove() leaves the next LoadBoard() returning a bare pointer


def _fp_swap(board):
    fp = next(f for f in board.GetFootprints() if f.GetReference() == "C1")
    fp.SetFPID(pcbnew.LIB_ID("Capacitor_SMD", "C_9999_Wrong"))


def _track_short(board):
    """A track from one pad of a 2-pad part straight onto its other pad (another net)."""
    for t in list(board.GetTracks()):       # bare board: the short is the only track
        board.RemoveNative(t)
    for fp in board.GetFootprints():
        pads = [p for p in fp.Pads() if p.IsOnLayer(pcbnew.F_Cu) and p.GetNetCode()]
        if len(pads) == 2 and pads[0].GetNetCode() != pads[1].GetNetCode():
            t = pcbnew.PCB_TRACK(board)
            t.SetStart(pads[0].GetPosition())
            t.SetEnd(pads[1].GetPosition())
            t.SetLayer(pcbnew.F_Cu)
            t.SetWidth(pcbnew.FromMM(0.15))
            t.SetNet(pads[0].GetNet())
            board.Add(t)
            return
    raise SystemExit("selftest: no 2-pad part on F.Cu to short")


def _track_delete(board):
    """Take one signal track out of the routed board: its net falls in two."""
    tracks = [t for t in board.GetTracks()
              if t.GetClass() != "PCB_VIA" and t.GetNetname() not in ("GND", "")]
    if not tracks:
        raise SystemExit("selftest: the board has no track to delete (route it first)")
    board.RemoveNative(max(tracks, key=lambda t: t.GetLength()))


def _thin_long_5v(width_mm):
    def mutate(board):
        """Thin the longest +5V segment (longer than a neck can be) to `width_mm`."""
        t = max((t for t in board.GetTracks()
                 if t.GetClass() != "PCB_VIA" and t.GetNetname() == "+5V"),
                key=lambda t: t.GetLength())
        if pcbnew.ToMM(t.GetLength()) <= NECK_MAX_MM:
            raise SystemExit("selftest: no +5V segment longer than a neck")
        t.SetWidth(pcbnew.FromMM(width_mm))
    return mutate


# name -> (mutation, needs the DRC, text its failure must contain)
MUTATIONS = {
    "pad on another net": (_pad_move, False, "parity: R"),
    "footprint deleted": (_fp_delete, False, "footprint C1 is in the netlist but missing"),
    "wrong footprint id": (_fp_swap, False, "C1 footprint is"),
    "track across two nets": (_track_short, True, "DRC shorting_items"),
    "track deleted": (_track_delete, True, "DRC unconnected"),
    "+5V thinned to 0.15": (_thin_long_5v(0.15), False, "power width: +5V track 0.150"),
    "+5V long run at 0.3": (_thin_long_5v(0.3), False, "power width: +5V track 0.300"),
}


def _mutate(path: Path, mutate) -> None:
    """Load `path`, plant a fault, save it back."""
    board = pcbnew.LoadBoard(str(path))
    mutate(board)
    pcbnew.SaveBoard(str(path), board)


def selftest(board_path: Path, nl: Netlist) -> int:
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, (mutate, drc, expect) in MUTATIONS.items():
            d = Path(tmp) / name.replace(" ", "_")
            d.mkdir()
            copy = _copy_project(board_path, d)
            _mutate(copy, mutate)
            board = pcbnew.LoadBoard(str(copy))
            found = (problems_parity(board, nl)
                     + problems_width(board, copy.with_suffix(".kicad_pro")))
            if drc:
                found += problems_drc(copy)
            if not any(expect in p for p in found):
                print(f"selftest: planted fault '{name}' was not found as {expect!r} "
                      f"(got {found[:3]})")
                bad += 1
    if bad:
        return 1
    print("HW DRC SELFTEST OK")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("board", type=Path)
    ap.add_argument("--netlist", type=Path, required=True)
    ap.add_argument("--selftest", action="store_true",
                    help="plant each fault in a copy of BOARD; every one must be found")
    a = ap.parse_args(argv)
    nl = load(a.netlist)
    if a.selftest:
        return selftest(a.board, nl)
    found = problems(a.board, nl)
    for p in found:
        print(f"HW DRC: {p}")
    if found:
        print(f"HW DRC: {len(found)} problems")
        return 1
    print(f"HW DRC: {warning_count(a.board)} warnings (not gated)")
    print("HW DRC OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
