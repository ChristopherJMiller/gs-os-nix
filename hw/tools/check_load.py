# SPDX-License-Identifier: GPL-3.0-or-later
"""hw-load: every Apple II slot pin presents at most 15 pF (Apple IIe HWR 5-13).

    python3 hw/tools/check_load.py --netlist NET BOARD.kicad_pcb    # table, then HW LOAD OK
    python3 hw/tools/check_load.py --selftest                       # HW LOAD SELFTEST OK

A card may load each slot pin with at most 15 pF (and two LS loads). The estimate for one
J1 net is its routed copper, in mm, times PF_PER_MM, plus the capacitance of every IC pin
on it, from hw/carrier/pin_capacitance.toml (datasheet Ci for an input, Cio for an I/O or
output pin; a pin the table does not list, a part value it lacks, a J1 net with no copper
on the board, or a part that is not a U, R, C or J is a failure, never a silent zero). The
SAFE and UART headers count 1 pF a pin, other passives 0. Apple's other limit, two LS loads,
is moot for LVC/ALVC inputs (about 1 uA against an LS load's 0.4 mA IIL / 20 uA IIH) and is
not gated.

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
                  if _norm(n) not in POWER_NETS and any(r == J1 for r, _ in nodes))


def _group(nl: Netlist, net: str) -> list[str]:
    """`net` and every net joined to it through 0 ohm parts (never into a power net)."""
    seen, todo = [net], [net]
    while todo:
        cur = todo.pop()
        for ref, _ in sorted(nl.nets[cur]):
            if _is_zero_ohm(nl, ref):
                other = through(nl, ref, cur)
                if other not in seen and _norm(other) not in POWER_NETS:
                    seen.append(other)
                    todo.append(other)
    return seen


def ic_values_on_j1_nets(nl: Netlist) -> set[str]:
    """Part values of every IC with a pin on a J1 net or on a net joined to one by 0 ohm."""
    return {nl.parts[r].value for n in j1_nets(nl) for m in _group(nl, n)
            for r, _ in nl.nets[m] if _is_ic(r)}


def _norm(name: str) -> str:
    """A net name as both sides compare it: KiCad writes '/NAME' for a root-sheet net."""
    return name.lstrip("/")


def _copper_mm(board) -> dict[str, float]:
    mm: dict[str, float] = {}
    for t in board.GetTracks():
        if t.Type() == pcbnew.PCB_VIA_T:
            continue
        name = _norm(t.GetNetname())
        mm[name] = mm.get(name, 0.0) + pcbnew.ToMM(t.GetLength())
    return mm


def _kind(ref: str) -> str:
    """ic (U), zero-or-other resistor (R), passive (C), connector (J, JP), or unknown."""
    prefix = ref.rstrip("0123456789")
    return {"U": "ic", "R": "res", "C": "cap", "J": "conn", "JP": "conn"}.get(prefix, "unknown")


def _pin_pf(caps: dict, value: str, pin: str) -> float | None:
    """The capacitance of pin `pin` of part `value`, or None if the table does not list it."""
    e = caps[value]
    for pins, cap in (("in_pins", "ci"), ("io_pins", "cio"), ("od_pins", "co")):
        if int(pin) in e.get(pins, []):
            return e[cap]
    return None


def table(board, nl: Netlist, caps: dict, pf_per_mm: float = PF_PER_MM) -> list[dict]:
    """One row per J1 signal net: length (mm), copper and pins (pF), total, the number of IC
    and header pins, whether 0 ohm parts joined other nets to it, and the faults found:
    `nocopper` (the net itself has no track on the board), `missing` ((ref, value) with no
    table entry), `unlisted` ((ref, pin, value) not in the part's pin lists) and `unknown`
    (refs of an unrecognised kind of part)."""
    mm = _copper_mm(board)
    rows = []
    for net in j1_nets(nl):
        group = _group(nl, net)
        length = sum(mm.get(_norm(m), 0.0) for m in group)
        pins, n_ic, n_hdr = 0.0, 0, 0
        missing, unlisted, unknown = set(), [], set()
        for m in group:
            for ref, pin in sorted(nl.nets[m]):
                if ref == J1:
                    continue
                kind = _kind(ref)
                if kind == "ic":
                    n_ic += 1
                    value = nl.parts[ref].value
                    if value not in caps:
                        missing.add((ref, value))
                        continue
                    pf = _pin_pf(caps, value, pin)
                    if pf is None:
                        unlisted.append((ref, pin, value))
                    else:
                        pins += pf
                elif kind == "conn" and _is_header(nl, ref):
                    n_hdr += 1
                    pins += HEADER_PF
                elif kind == "unknown":
                    unknown.add(ref)
        copper = length * pf_per_mm
        rows.append({"net": net, "length": length, "copper": copper, "pins": pins,
                     "total": copper + pins, "ic_pins": n_ic, "header_pins": n_hdr,
                     "joined": len(group) > 1,
                     "nocopper": mm.get(_norm(net), 0.0) <= 0.0,
                     "missing": sorted(missing), "unlisted": unlisted,
                     "unknown": sorted(unknown)})
    return rows


def problems(board, nl: Netlist, caps: dict, limit_pf: float = LIMIT_PF,
             pf_per_mm: float = PF_PER_MM) -> list[str]:
    out, reported = [], set()
    for r in table(board, nl, caps, pf_per_mm):
        net = r["net"]
        if r["nocopper"]:
            out.append(f"{net}: no copper on the board (no net of that name, or no tracks "
                       "on it): the load would read as zero")
        for ref, value in r["missing"]:
            if value not in reported:
                reported.add(value)
                out.append(f"{ref}: no capacitance entry for part value '{value}' "
                           "in pin_capacitance.toml")
        for ref, pin, value in r["unlisted"]:
            out.append(f"{ref}.{pin} ({value}) on {net} is not in the pin table's input, "
                       "I/O or open-drain pins")
        for ref in r["unknown"]:
            out.append(f"{ref}: unknown part (not U, R, C or J) on {net}; the gate cannot "
                       "say what it loads")
        if not (r["ic_pins"] or r["header_pins"] or r["joined"]):
            out.append(f"{net}: no IC or header pin on it, so nothing to gate (is the "
                       "netlist right?)")
        if r["total"] > limit_pf + 1e-9:
            out.append(f"{net}: {r['total']:.1f} pF over the {limit_pf:.1f} pF limit "
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
    walk stops there), a header net, an open-drain output and a daisy chain through a 0 ohm
    part."""
    del caps
    P = PartInfo
    parts = {
        "J1": P("Apple II slot", "gsnix:edge", {}),
        "U1": P("SN74LVCR2245A", "Package_SO:SSOP-20", {}),
        "U4": P("74LVC541", "Package_SO:TSSOP-20", {}),
        "U5": P("74LVC541", "Package_SO:TSSOP-20", {}),
        "U6": P("74LVC2G17", "Package_TO_SOT_SMD:SOT-363", {}),
        "U7": P("74LVC1G07", "Package_TO_SOT_SMD:SOT-23-5", {}),
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
        "SLOT_X": {("J1", "4"), ("R2", "1"), ("U6", "3")},
        "x_far": {("R2", "2"), ("U5", "9")},
        "SLOT_JP": {("J1", "5"), ("JP1", "1")},
        "SLOT_CH_IN": {("J1", "6"), ("R3", "1")},
        "SLOT_CH_OUT": {("J1", "7"), ("R3", "2")},
        "SLOT_OD": {("J1", "8"), ("U7", "4")},
        "+5V_SLOT": {("J1", "25")},
        "GND": {("J1", "26")},
    }
    board = empty_board()
    for net, mm, y in [("SLOT_D0", 30, 1), ("SLOT_D1", 50, 2), ("SLOT_PH0", 20, 3),
                       ("ph0_in", 10, 4), ("SLOT_X", 100, 5), ("x_far", 500, 6),
                       ("SLOT_JP", 5, 7), ("SLOT_CH_IN", 10, 8), ("SLOT_CH_OUT", 10, 9),
                       ("SLOT_OD", 10, 11), ("+5V_SLOT", 400, 10)]:
        _track(board, net, mm, y)
    return board, Netlist(parts=parts, nets=nets)


# --- the planted faults: each edits the synthetic (board, netlist, table) in place ---------

def _lengthen(board, nl, caps):
    t = next(t for t in board.GetTracks() if t.GetNetname() == "SLOT_D0")
    e = t.GetEnd()
    t.SetEnd(pcbnew.VECTOR2I(e.x + pcbnew.FromMM(60.0), e.y))


def _third_buffer(board, nl, caps):
    nl.parts["U9"] = PartInfo("74LVC541", "Package_SO:TSSOP-20", {})
    nl.nets["SLOT_D0"].add(("U9", "2"))


def _raise_ci(board, nl, caps):
    caps["74LVC541"]["ci"] = 20.0


def _no_tracks(board, nl, caps):
    for t in [t for t in board.GetTracks() if t.GetNetname() == "SLOT_D1"]:
        board.RemoveNative(t)


def _drop_entry(board, nl, caps):
    del caps["74LVC541"]


def _output_pin(board, nl, caps):
    nl.nets["SLOT_D1"].add(("U6", "4"))     # a 2G17 push-pull output on a slot net


def _unknown_part(board, nl, caps):
    nl.parts["Q1"] = PartInfo("2N7002", "Package_TO_SOT_SMD:SOT-23", {})
    nl.nets["SLOT_D1"].add(("Q1", "1"))


def _no_ic(board, nl, caps):
    nl.nets["SLOT_D1"] = {("J1", "2")}


PLANT = {
    "track lengthened by 60 mm": _lengthen,
    "third buffer input on SLOT_D0": _third_buffer,
    "74LVC541 Ci raised to 20 pF": _raise_ci,
    "no tracks on SLOT_D1": _no_tracks,
    "74LVC541 missing from the table": _drop_entry,
    "2G17 output pin on SLOT_D1": _output_pin,
    "unknown part Q1 on SLOT_D1": _unknown_part,
    "no IC pin on SLOT_D1": _no_ic,
}
# name -> text its failure must contain; each names the cause and no other fault's message has it
EXPECT = {
    "track lengthened by 60 mm": "over 90.0 mm",
    "third buffer input on SLOT_D0": "3 IC pins",
    "74LVC541 Ci raised to 20 pF": "2 IC pins 25.5 pF",
    "no tracks on SLOT_D1": "SLOT_D1: no copper",
    "74LVC541 missing from the table": "no capacitance entry for part value '74LVC541'",
    "2G17 output pin on SLOT_D1": "U6.4 (74LVC2G17) on SLOT_D1 is not in the pin table",
    "unknown part Q1 on SLOT_D1": "Q1: unknown part",
    "no IC pin on SLOT_D1": "SLOT_D1: no IC or header pin",
}


def selftest(caps: dict) -> int:
    board, nl = synthetic(caps)
    clean = problems(board, nl, caps)
    if clean:
        print("selftest: the synthetic baseline is not clean:\n  " + "\n  ".join(clean))
        return 1
    failed = 0
    for name, plant in PLANT.items():
        b, n = synthetic(caps)
        c = copy.deepcopy(caps)
        plant(b, n, c)
        found = problems(b, n, c)
        if not any(EXPECT[name] in p for p in found):
            print(f"selftest: planted fault '{name}' was not found as "
                  f"{EXPECT[name]!r} (got {found[:3]})")
            failed += 1
        if any(text in p for other, text in EXPECT.items() if other != name for p in found):
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
