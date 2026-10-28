# SPDX-License-Identifier: GPL-3.0-or-later
"""Pin-map gate: pins.toml <-> carrier_top's ports <-> the netlist <-> the LPF <-> Trellis.

A ball that is wrong in any one of these still builds: the LPF names a real site, the
netlist is a valid netlist, the RTL has ports. The board is then silently miswired, so
this reads all five and asserts they agree:

  1. pins.toml's ports equal `carrier_top`'s ports in name, direction and width;
  2. every ball is a PIO of the package in Trellis's database, in a bank the module
     map says is 3.3 V;
  3. for every bit that is not on the module, the net on J2.<SODIMM pin of that ball>
     is that bit's net name (J2 is the SODIMM socket, so this is the carrier's wiring
     from the module's ball to the port the bus section sees);
  4. the LPF has exactly one LOCATE per bit, on that bit's ball.

    check_pinmap.py --pins P.toml --module M.toml --netlist N.net --verilog V.v --lpf L.lpf
    check_pinmap.py --selftest ...      (the same arguments)

`--selftest` plants each fault in a copy of the inputs and requires the check to fail.
"""
import argparse
import copy
import json
import os
import re
import sys
import tempfile
from pathlib import Path
from typing import Callable

if __package__ in (None, ""):  # run as a script: put the repo root on sys.path
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from hw.tools import module as modmod  # noqa: E402
from hw.tools import netlist as nlmod  # noqa: E402
from hw.tools import pins as pinsmod  # noqa: E402

TOP = "carrier_top"
SODIMM = "J2"
PACKAGES = {"BG256": "CABGA256"}  # the part number's package code -> Trellis's name
VERILOG_DIR = {"in": "input", "out": "output", "inout": "inout"}
_LOCATE = re.compile(r'^LOCATE\s+COMP\s+"([^"]+)"\s+SITE\s+"([^"]+)"\s*;', re.M)


class Trellis:
    """The Trellis database's view of one package: ball -> bank, for the PIO balls."""

    def __init__(self, part: str) -> None:
        device = "-".join(part.split("-")[:2])  # LFE5U-25F-6BG256C -> LFE5U-25F
        code = re.search(r"(BG\d+)", part)
        if not code or code.group(1) not in PACKAGES:
            raise ValueError(f"cannot tell the package of {part!r}")
        db = json.loads((Path(os.environ["TRELLIS_DB"]) / "ECP5" / device
                         / "iodb.json").read_text())
        bank = {(e["row"], e["col"], e["pio"]): e["bank"] for e in db["pio_metadata"]}
        self.bank: dict[str, int] = {
            ball: bank[(c["row"], c["col"], c["pio"])]
            for ball, c in db["packages"][PACKAGES[code.group(1)]].items()}


def problems(pm: pinsmod.PinMap, mod: modmod.ModuleMap, nl: nlmod.Netlist,
             vports: dict[str, tuple[str, int]], lpf: str, tre: Trellis) -> list[str]:
    out: list[str] = []
    # 1. ports: name, direction, width
    want = {n: (VERILOG_DIR[p.dir], len(p.balls)) for n, p in pm.ports.items()}
    for n in sorted(want.keys() - vports.keys()):
        out.append(f"pins.toml has port {n}, which {TOP} does not")
    for n in sorted(vports.keys() - want.keys()):
        out.append(f"{TOP} has port {n}, which pins.toml does not")
    for n in sorted(want.keys() & vports.keys()):
        if want[n] != vports[n]:
            out.append(f"port {n}: pins.toml says {want[n]}, {TOP} says {vports[n]} "
                       f"(direction, width)")
    # 2. balls: PIO of the package, 3.3 V bank
    for p in pm.ports.values():
        for net, _, ball in pinsmod.bits(p):
            if ball not in tre.bank:
                out.append(f"{net}: ball {ball} is not a PIO of the package")
            elif mod.bank_vccio.get(tre.bank[ball]) != 3.3:
                out.append(f"{net}: ball {ball} is in bank {tre.bank[ball]}, which the "
                           f"module powers at {mod.bank_vccio.get(tre.bank[ball])} V")
    # 3. the netlist: J2.<pin> carries the bit's net
    for p in pm.ports.values():
        if p.on_module:
            continue
        for net, _, ball in pinsmod.bits(p):
            try:
                pin = modmod.pin_for_ball(mod, ball)
            except KeyError:
                out.append(f"{net}: ball {ball} is not on the SODIMM, so no carrier "
                           f"wire can reach it")
                continue
            got = nlmod.net_of(nl, SODIMM, str(pin))
            if got != net:
                out.append(f"{net}: ball {ball} is {SODIMM}.{pin}, whose net is "
                           f"{got!r}")
    # 4. the LPF: one LOCATE per bit, on the right ball
    seen: dict[str, list[str]] = {}
    for name, ball in _LOCATE.findall(lpf):
        seen.setdefault(name, []).append(ball)
    expect = {lpf_name: ball for p in pm.ports.values()
              for _, lpf_name, ball in pinsmod.bits(p)}
    for name, ball in sorted(expect.items()):
        got = seen.get(name, [])
        if len(got) != 1:
            out.append(f"LPF has {len(got)} LOCATE lines for {name}, not one")
        elif got[0] != ball:
            out.append(f"LPF puts {name} on {got[0]}, pins.toml on {ball}")
    for name in sorted(seen.keys() - expect.keys()):
        out.append(f"LPF locates {name}, which is not a bit of any port")
    return out


# --- planted faults ----------------------------------------------------------

class Inputs:
    """A mutable copy of everything the check reads."""

    def __init__(self, pm, mod, nl, verilog: str, lpf: str) -> None:
        self.pm, self.mod, self.nl = pm, mod, nl
        self.verilog, self.lpf = verilog, lpf


def _first_ball(i: Inputs, port: str) -> str:
    return i.pm.ports[port].balls[0]


def _swap_sodimm_nodes(i: Inputs) -> None:
    a = modmod.pin_for_ball(i.mod, i.pm.ports["slot_a"].balls[0])
    b = modmod.pin_for_ball(i.mod, i.pm.ports["slot_a"].balls[1])
    na, nb = nlmod.net_of(i.nl, SODIMM, str(a)), nlmod.net_of(i.nl, SODIMM, str(b))
    i.nl.nets[na].discard((SODIMM, str(a)))
    i.nl.nets[nb].discard((SODIMM, str(b)))
    i.nl.nets[na].add((SODIMM, str(b)))
    i.nl.nets[nb].add((SODIMM, str(a)))


def _rename_port(i: Inputs) -> None:
    i.verilog = re.sub(r"\bslot_rw\b", "slot_rww", i.verilog, count=1)


def _flip_direction(i: Inputs) -> None:
    i.verilog = re.sub(r"\binput(\s+wire\s+(?:\[[^\]]*\]\s+)?slot_rw\b)", r"output\1",
                       i.verilog, count=1)


def _widen_port(i: Inputs) -> None:
    i.verilog = re.sub(r"\[15:0\](\s+slot_a\b)", r"[16:0]\1", i.verilog, count=1)


def _non_pio_ball(i: Inputs) -> None:
    i.pm.ports["slot_rw"].balls = ["F7"]  # a VCCIO0 power ball: no PIO, not on the SODIMM


def _ball_off_sodimm(i: Inputs) -> None:
    i.pm.ports["slot_rw"].balls = ["T15"]  # a real PIO ball no SODIMM pin carries
    assert "T15" not in i.mod.balls.values()


def _lpf_drops_a_bit(i: Inputs) -> None:
    i.lpf = re.sub(r'^LOCATE COMP "slot_rw"[^\n]*\n', "", i.lpf, count=1, flags=re.M)


def _lpf_duplicates_a_bit(i: Inputs) -> None:
    i.lpf += 'LOCATE COMP "slot_rw" SITE "A1";\n'


def _lpf_wrong_ball(i: Inputs) -> None:
    i.lpf = re.sub(r'(LOCATE COMP "slot_rw" SITE ")[^"]+', r"\1T15", i.lpf, count=1)


# name -> (the fault, text the resulting problem must contain): a fault that trips some
# other rule would hide a dead one.
MUTATIONS: dict[str, tuple[Callable[[Inputs], None], str]] = {
    "swap_two_sodimm_nodes": (_swap_sodimm_nodes, "whose net is"),
    "rename_a_port": (_rename_port, "which carrier_top does not"),
    "flip_a_direction": (_flip_direction, "(direction, width)"),
    "widen_a_port": (_widen_port, "(direction, width)"),
    "ball_is_a_power_ball": (_non_pio_ball, "is not a PIO of the package"),
    "ball_not_on_the_sodimm": (_ball_off_sodimm, "is not on the SODIMM"),
    "lpf_drops_a_bit": (_lpf_drops_a_bit, "0 LOCATE lines"),
    "lpf_duplicates_a_bit": (_lpf_duplicates_a_bit, "2 LOCATE lines"),
    "lpf_wrong_ball": (_lpf_wrong_ball, "LPF puts"),
}


def _check(i: Inputs, tre: Trellis) -> list[str]:
    """`problems`, with the Verilog read back through the real parser (from a file,
    as the gate reads it)."""
    with tempfile.TemporaryDirectory() as d:
        v = Path(d) / "carrier_top.v"
        v.write_text(i.verilog)
        vports = pinsmod.verilog_ports(v, TOP)
    return problems(i.pm, i.mod, i.nl, vports, i.lpf, tre)


def selftest(i: Inputs, tre: Trellis) -> list[str]:
    fails = [f"base is not clean: {p}" for p in _check(i, tre)]
    for name, (mutate, needle) in MUTATIONS.items():
        bad = Inputs(copy.deepcopy(i.pm), i.mod, copy.deepcopy(i.nl), i.verilog, i.lpf)
        try:
            mutate(bad)
        except (KeyError, IndexError, AssertionError) as e:
            fails.append(f"mutation {name} could not be applied: {e!r}")
            continue
        if (bad.verilog, bad.lpf) == (i.verilog, i.lpf) and bad.pm == i.pm \
                and bad.nl == i.nl:
            fails.append(f"mutation {name} changed nothing")
        elif not any(needle in p for p in _check(bad, tre)):
            fails.append(f"mutation {name} did not trip the rule that says {needle!r}")
    return fails


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    for a in ("pins", "module", "netlist", "verilog", "lpf"):
        ap.add_argument(f"--{a}", type=Path, required=True)
    ap.add_argument("--selftest", action="store_true",
                    help="plant each fault in a copy and require the check to fail")
    args = ap.parse_args(argv)
    pm = pinsmod.load(args.pins)
    i = Inputs(pm, modmod.load(args.module), nlmod.load(args.netlist),
               args.verilog.read_text(), args.lpf.read_text())
    tre = Trellis(pm.part)
    if args.selftest:
        fails = selftest(i, tre)
        for f in fails:
            print(f"SELFTEST FAIL: {f}")
        if fails:
            return 1
        print(f"{len(MUTATIONS)} planted faults, each caught")
        print("HW PINMAP SELFTEST OK")
        return 0
    probs = _check(i, tre)
    for p in probs:
        print(f"PINMAP: {p}")
    if probs:
        return 1
    print("HW PINMAP OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
