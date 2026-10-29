# SPDX-License-Identifier: GPL-3.0-or-later
"""hw-load: every Apple II slot pin presents at most 15 pF (Apple IIe HWR 5-13).

    python3 hw/tools/check_load.py --netlist NET BOARD.kicad_pcb    # table, then HW LOAD OK
    python3 hw/tools/check_load.py --selftest                       # HW LOAD SELFTEST OK

A card may load each slot pin with at most 15 pF (and two LS loads). The estimate for one
J1 net is its routed copper, in mm, times PF_PER_MM, plus the capacitance of every IC pin
on it, from hw/carrier/pin_capacitance.toml (datasheet Ci for an input, Cio for an I/O or
output pin). The SAFE and UART headers count 1 pF a pin, other passives 0.

Which pins sit on a net comes from the netlist; the copper comes from the board's own
tracks, by the net name KiCad gives them. A 0 ohm resistor (value "0", "0R" or "0 ohm")
joins its two nets: the clock fingers reach their 2G17s through one, and the copper and pins
beyond it are the same load. Any other resistor ends the walk. The power nets are exempt.

The selftest runs on a small synthetic board that passes (the routed carrier does not, until
its re-route), and plants three faults on fresh copies of it; each must fail with its own
message.
"""
import argparse
import copy
import sys
from pathlib import Path

import pcbnew
import tomllib

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from hw.tools.netlist import Netlist, PartInfo, load, through

LIMIT_PF = 15.0
PF_PER_MM = 0.10
HEADER_PF = 1.0
J1 = "J1"
POWER_NETS = ("+5V_SLOT", "GND", "-12V_SLOT", "-5V_SLOT", "+12V_SLOT")
ZERO_OHM = ("0", "0R", "0Ω", "0 ohm", "0 Ω")
DEFAULT_TABLE = Path(__file__).resolve().parents[1] / "carrier" / "pin_capacitance.toml"


def load_caps(path: Path = DEFAULT_TABLE) -> dict:
    return tomllib.loads(Path(path).read_text())["parts"]


def _is_ic(ref: str) -> bool:
    return ref.startswith("U")


def _is_header(nl: Netlist, ref: str) -> bool:
    fp = nl.parts[ref].footprint
    return "PinHeader" in fp or "PinSocket" in fp


def _is_zero_ohm(nl: Netlist, ref: str) -> bool:
    return ref.startswith("R") and nl.parts[ref].value in ZERO_OHM


def j1_nets(nl: Netlist) -> list[str]:
    return sorted(n for n, nodes in nl.nets.items()
                  if n not in POWER_NETS and any(r == J1 for r, _ in nodes))


def _group(nl: Netlist, net: str) -> list[str]:
    """`net` and every net joined to it through 0 ohm parts (never into a power net)."""
    seen, todo = [net], [net]
    while todo:
        cur = todo.pop()
        for ref, _ in sorted(nl.nets[cur]):
            if _is_zero_ohm(nl, ref):
                other = through(nl, ref, cur)
                if other not in seen and other not in POWER_NETS:
                    seen.append(other)
                    todo.append(other)
    return seen


def ic_values_on_j1_nets(nl: Netlist) -> set[str]:
    """Part values of every IC with a pin on a J1 net or on a net joined to one by 0 ohm."""
    return {nl.parts[r].value for n in j1_nets(nl) for m in _group(nl, n)
            for r, _ in nl.nets[m] if _is_ic(r)}


def _copper_mm(board) -> dict[str, float]:
    mm: dict[str, float] = {}
    for t in board.GetTracks():
        if t.Type() == pcbnew.PCB_VIA_T:
            continue
        name = t.GetNetname().lstrip("/")
        mm[name] = mm.get(name, 0.0) + pcbnew.ToMM(t.GetLength())
    return mm


def _pin_pf(caps: dict, value: str, pin: str) -> float | None:
    e = caps.get(value)
    if e is None:
        return None
    if int(pin) in e.get("io_pins", []):
        return e["cio"]
    return e["ci"]


def table(board, nl: Netlist, caps: dict, pf_per_mm: float = PF_PER_MM) -> list[dict]:
    """One row per J1 signal net: length (mm), copper and pins (pF), total, the number of
    IC pins, and `missing`, the (ref, value) of ICs with no table entry."""
    mm = _copper_mm(board)
    rows = []
    for net in j1_nets(nl):
        group = _group(nl, net)
        length = sum(mm.get(m, 0.0) for m in group)
        pins, n_ic, missing = 0.0, 0, set()
        for m in group:
            for ref, pin in sorted(nl.nets[m]):
                if ref == J1:
                    continue
                if _is_ic(ref):
                    n_ic += 1
                    pf = _pin_pf(caps, nl.parts[ref].value, pin)
                    if pf is None:
                        missing.add((ref, nl.parts[ref].value))
                    else:
                        pins += pf
                elif _is_header(nl, ref):
                    pins += HEADER_PF
        copper = length * pf_per_mm
        rows.append({"net": net, "length": length, "copper": copper, "pins": pins,
                     "total": copper + pins, "ic_pins": n_ic, "missing": sorted(missing)})
    return rows


def problems(board, nl: Netlist, caps: dict, limit_pf: float = LIMIT_PF,
             pf_per_mm: float = PF_PER_MM) -> list[str]:
    out, reported = [], set()
    for r in table(board, nl, caps, pf_per_mm):
        for ref, value in r["missing"]:
            if value not in reported:
                reported.add(value)
                out.append(f"{ref}: no capacitance entry for part value '{value}' "
                           "in pin_capacitance.toml")
        if r["total"] > limit_pf + 1e-9:
            out.append(f"{r['net']}: {r['total']:.1f} pF over the {limit_pf:.1f} pF limit "
                       f"(copper {r['copper']:.1f} pF over {r['length']:.1f} mm + "
                       f"{r['ic_pins']} IC pins {r['pins']:.1f} pF)")
    return out


def print_table(rows: list[dict], limit_pf: float = LIMIT_PF) -> None:
    print(f"{'net':<16}{'mm':>8}{'copper':>9}{'pins':>8}{'total':>8}{'limit':>8}")
    for r in sorted(rows, key=lambda r: (-r["total"], r["net"])):
        flag = "  FAIL" if r["total"] > limit_pf + 1e-9 else ""
        print(f"{r['net']:<16}{r['length']:8.1f}{r['copper']:9.1f}{r['pins']:8.1f}"
              f"{r['total']:8.1f}{limit_pf:8.1f}{flag}")


# --- the synthetic board and the selftest's planted faults --------------------------

def empty_board():
    return pcbnew.BOARD()


def _track(board, net: str, mm: float, y: float) -> None:
    ni = board.FindNet(net)
    if ni is None:
        ni = pcbnew.NETINFO_ITEM(board, net)
        board.Add(ni)
    t = pcbnew.PCB_TRACK(board)
    t.SetStart(pcbnew.VECTOR2I(0, pcbnew.FromMM(y)))
    t.SetEnd(pcbnew.VECTOR2I(pcbnew.FromMM(mm), pcbnew.FromMM(y)))
    t.SetWidth(pcbnew.FromMM(0.15))
    t.SetLayer(pcbnew.F_Cu)
    t.SetNet(ni)
    board.Add(t)


def synthetic(caps: dict):
    """A small board and netlist that pass: data nets D0/D1 (a translator pin and a 541
    input each), a clock through a 0 ohm part to a 2G17, a net behind a 1k resistor (the
    walk stops there), a header net and a daisy chain through a 0 ohm part."""
    del caps
    P = PartInfo
    parts = {
        "J1": P("Apple II slot", "gsnix:edge", {}),
        "U1": P("SN74LVCR2245A", "Package_SO:SSOP-20", {}),
        "U4": P("74LVC541", "Package_SO:TSSOP-20", {}),
        "U5": P("74LVC541", "Package_SO:TSSOP-20", {}),
        "U6": P("74LVC2G17", "Package_TO_SOT_SMD:SOT-363", {}),
        "R1": P("0", "Resistor_SMD:R_0603", {}),
        "R2": P("1k", "Resistor_SMD:R_0603", {}),
        "R3": P("0", "Resistor_SMD:R_0603", {}),
        "JP1": P("SAFE", "Connector_PinHeader_2.54mm:PinHeader_1x02", {}),
    }
    nets = {
        "SLOT_D0": {("J1", "1"), ("U1", "18"), ("U4", "2")},
        "SLOT_D1": {("J1", "2"), ("U1", "17"), ("U4", "3")},
        "SLOT_PH0": {("J1", "3"), ("R1", "1")},
        "ph0_in": {("R1", "2"), ("U6", "1")},
        "SLOT_X": {("J1", "4"), ("R2", "1")},
        "x_far": {("R2", "2"), ("U5", "9")},
        "SLOT_JP": {("J1", "5"), ("JP1", "1")},
        "SLOT_CH_IN": {("J1", "6"), ("R3", "1")},
        "SLOT_CH_OUT": {("J1", "7"), ("R3", "2")},
        "+5V_SLOT": {("J1", "25")},
        "GND": {("J1", "26")},
    }
    board = empty_board()
    for net, mm, y in [("SLOT_D0", 30, 1), ("SLOT_D1", 50, 2), ("SLOT_PH0", 20, 3),
                       ("ph0_in", 10, 4), ("SLOT_X", 100, 5), ("x_far", 500, 6),
                       ("SLOT_JP", 5, 7), ("SLOT_CH_IN", 10, 8), ("SLOT_CH_OUT", 10, 9),
                       ("+5V_SLOT", 400, 10)]:
        _track(board, net, mm, y)
    return board, Netlist(parts=parts, nets=nets)


def lengthen_track(board, net: str, extra_mm: float) -> None:
    t = next(t for t in board.GetTracks() if t.GetNetname() == net)
    e = t.GetEnd()
    t.SetEnd(pcbnew.VECTOR2I(e.x + pcbnew.FromMM(extra_mm), e.y))


def add_third_buffer(nl: Netlist, net: str) -> None:
    nl.parts["U9"] = PartInfo("74LVC541", "Package_SO:TSSOP-20", {})
    nl.nets[net].add(("U9", "2"))


# name -> text the failure must contain (the net's total, so each fault is its own message)
EXPECT = {
    "track lengthened by 60 mm": "SLOT_D0: 18.5 pF",
    "third buffer input on SLOT_D0": "SLOT_D0: 16.5 pF",
    "74LVC541 Ci raised to 20 pF": "SLOT_D0: 28.5 pF",
}


def selftest(caps: dict) -> int:
    board, nl = synthetic(caps)
    clean = problems(board, nl, caps)
    if clean:
        print("selftest: the synthetic baseline is not clean:\n  " + "\n  ".join(clean))
        return 1
    cases = {}
    b, n = synthetic(caps)
    lengthen_track(b, "SLOT_D0", 60.0)
    cases["track lengthened by 60 mm"] = problems(b, n, caps)
    b, n = synthetic(caps)
    add_third_buffer(n, "SLOT_D0")
    cases["third buffer input on SLOT_D0"] = problems(b, n, caps)
    b, n = synthetic(caps)
    bad = copy.deepcopy(caps)
    bad["74LVC541"]["ci"] = 20.0
    cases["74LVC541 Ci raised to 20 pF"] = problems(b, n, bad)
    failed = 0
    for name, found in cases.items():
        if not any(EXPECT[name] in p for p in found):
            print(f"selftest: planted fault '{name}' was not found as "
                  f"{EXPECT[name]!r} (got {found[:3]})")
            failed += 1
        others = [v for k, v in EXPECT.items() if k != name]
        if any(o in p for o in others for p in found):
            print(f"selftest: planted fault '{name}' also produced another fault's message")
            failed += 1
    if failed:
        return 1
    print("HW LOAD SELFTEST OK")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("board", type=Path, nargs="?")
    ap.add_argument("--netlist", type=Path)
    ap.add_argument("--table", type=Path, default=DEFAULT_TABLE,
                    help="the pin capacitance table (default hw/carrier/pin_capacitance.toml)")
    ap.add_argument("--selftest", action="store_true",
                    help="plant each fault in a copy of a synthetic passing board")
    a = ap.parse_args(argv)
    caps = load_caps(a.table)
    if a.selftest:
        return selftest(caps)
    if a.board is None or a.netlist is None:
        ap.error("BOARD and --netlist are required")
    board = pcbnew.LoadBoard(str(a.board))
    nl = load(a.netlist)
    print_table(table(board, nl, caps))
    found = problems(board, nl, caps)
    for p in found:
        print(f"HW LOAD: {p}")
    if found:
        return 1
    print("HW LOAD OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
