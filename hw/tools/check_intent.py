# SPDX-License-Identifier: GPL-3.0-or-later
"""Design-intent check: what ERC and DRC cannot see.

ERC finds a floating input or two drivers on a net. It cannot find a translator that is
enabled before the FPGA configures, an interrupt daisy chain that is not passed through,
a 3.3 V buffer powered from the slot's 5 V, or slot +5 V reaching a load without the
fuse. Those are properties of the pull-up and pull-down resistors and the rails, so this
reads the netlist (never the board) and states each as a rule. Every rule has a planted
mutation in MUTATIONS that must trip it; `--selftest` runs them all.

    check_intent.py NETLIST [--module MAP.toml]
    check_intent.py --selftest NETLIST [--module MAP.toml]

With a module map (hw/carrier/icesugar_pro_v1_3.toml) the carrier's J2 rules apply too:
the module's 5V pins on +5V_MOD, its GND pins on GND, and +5V_MOD fed from +5V through
one series diode and nothing else.

Pin numbers below are the packages' own: SN74LVC8T245 (TSSOP-24), SN74ALVC164245
(TSSOP-48), SN74LVC1T45 (SOT-23-6); see the datasheets cited in hw/src/slot_bus.py.
"""
import argparse
import copy
import sys
from pathlib import Path
from typing import Callable

if __package__ in (None, ""):  # run as a script: put the repo root on sys.path
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from hw.tools import netlist as nlmod  # noqa: E402
from hw.tools.netlist import Netlist, PartInfo  # noqa: E402

SLOT_5V, FUSED_5V, V3V3, GND, MOD_5V = "+5V_SLOT", "+5V", "+3V3", "GND", "+5V_MOD"
LV_PARTS = ("74LVC541", "74LVC2G17", "74LVC1G07")  # 3.3 V parts: never on 5 V
OE_NETS = ("slot_d_oe_n", "slot_a_oe_n")        # pulled up: off until the FPGA says so
DIR_NETS = ("slot_d_dir", "slot_a_dir")         # pulled down: slot -> FPGA
RELEASED_NETS = ("slot_irq_n_o", "slot_nmi_n_o", "slot_inh_n_o", "slot_dma_n_o",
                 "slot_rdy_o")                  # open-drain inputs: pulled up = released
RESET_NET = "slot_reset_n_o"
NEG_RAIL_PINS = {"33": "-12V", "34": "-5V", "50": "+12V"}
CHAINS = (("28", "23", "INT IN -> INT OUT"), ("27", "24", "DMA IN -> DMA OUT"))
# (value, pin -> rail): which rail each supply pin of a dual-supply translator sits on.
TRANSLATOR_SUPPLY = {
    "SN74LVC8T245": {"1": V3V3, "23": FUSED_5V, "24": FUSED_5V},
    "74ALVC164245": {"31": V3V3, "42": V3V3, "7": FUSED_5V, "18": FUSED_5V},
    "SN74LVC1T45": {"1": V3V3, "6": FUSED_5V},
}


# --- netlist helpers ---------------------------------------------------------

def _pin_nets(nl: Netlist, ref: str) -> dict[str, str]:
    return {pin: name for name, nodes in nl.nets.items() for r, pin in nodes if r == ref}


def _refs_valued(nl: Netlist, value: str) -> list[str]:
    return sorted(r for r, p in nl.parts.items() if p.value == value)


def _is_zero(value: str) -> bool:
    return value.strip().lower().replace(" ", "").replace("ω", "").replace("Ω", "") in {
        "0", "0r", "0r0"}


def _resistors(nl: Netlist, net: str) -> list[str]:
    return [r for r in sorted(nlmod.neighbours(nl, net))
            if r.startswith("R") and r in nl.parts]


def _far_side(nl: Netlist, ref: str, net: str) -> str | None:
    try:
        return nlmod.through(nl, ref, net)
    except ValueError:
        return None


def _has_pull(nl: Netlist, net: str, rail: str) -> bool:
    """A real (non-zero-ohm) resistor from `net` to `rail`."""
    return any(not _is_zero(nl.parts[r].value) and _far_side(nl, r, net) == rail
               for r in _resistors(nl, net))


def _joined(nl: Netlist, a: str | None, b: str | None) -> bool:
    """Same net, or one 0-ohm part between them."""
    if a is None or b is None:
        return False
    if a == b:
        return True
    return any(_is_zero(nl.parts[r].value) and _far_side(nl, r, a) == b
               for r in _resistors(nl, a))


# --- the rules ---------------------------------------------------------------

def _rail_rules(nl: Netlist) -> list[str]:
    out = []
    # Slot +5 V reaches nothing but J1.25, the fuse, and capacitors.
    for ref, pin in sorted(nl.nets.get(SLOT_5V, set())):
        if (ref, pin) == ("J1", "25") or ref == "F1" or ref.startswith("C"):
            continue
        out.append(f"{SLOT_5V} reaches {ref}.{pin}: slot 5 V may feed only the fuse F1 "
                   f"and capacitors")
    if nlmod.net_of(nl, "J1", "25") != SLOT_5V:
        out.append(f"J1.25 is on {nlmod.net_of(nl, 'J1', '25')!r}, not {SLOT_5V}")
    if "F1" not in nlmod.neighbours(nl, SLOT_5V):
        out.append(f"the fuse F1 is not on {SLOT_5V}")
    elif _far_side(nl, "F1", SLOT_5V) != FUSED_5V:
        out.append(f"F1 does not run {SLOT_5V} -> {FUSED_5V}")
    # GND is slot pin 26 and no other.
    if nlmod.net_of(nl, "J1", "26") != GND:
        out.append(f"J1.26 is on {nlmod.net_of(nl, 'J1', '26')!r}, not GND "
                   f"(the slot's only ground pin)")
    for ref, pin in sorted(nl.nets.get(GND, set())):
        if ref == "J1" and pin != "26":
            out.append(f"GND lands on J1.{pin}; only J1.26 is ground")
    # +12 V, -5 V, -12 V: a bypass capacitor to GND and nothing else.
    for pin, name in sorted(NEG_RAIL_PINS.items()):
        net = nlmod.net_of(nl, "J1", pin)
        if net is None:
            out.append(f"J1.{pin} ({name}) is not on any net")
            continue
        others = sorted((r, p) for r, p in nl.nets[net] if (r, p) != ("J1", pin))
        caps = [r for r, _ in others if r.startswith("C")]
        for r, p in others:
            if not r.startswith("C"):
                out.append(f"J1.{pin} ({name}) reaches {r}.{p}: only a bypass "
                           f"capacitor may sit on it")
        if not caps:
            out.append(f"J1.{pin} ({name}) has no bypass capacitor")
        for c in caps:
            if _far_side(nl, c, net) != GND:
                out.append(f"{c} on J1.{pin} ({name}) does not go to GND")
    return out


def _chain_rules(nl: Netlist) -> list[str]:
    return [f"{label}: J1.{a} and J1.{b} are not joined directly or by a 0 ohm part; "
            f"every card past this one loses the signal"
            for a, b, label in CHAINS
            if not _joined(nl, nlmod.net_of(nl, "J1", a), nlmod.net_of(nl, "J1", b))]


def _powerup_rules(nl: Netlist) -> list[str]:
    out = []
    for net in OE_NETS:
        if not _has_pull(nl, net, V3V3):
            out.append(f"{net} has no pull-up resistor to {V3V3}: the translator is "
                       f"enabled before the FPGA configures")
    for net in DIR_NETS:
        if not _has_pull(nl, net, GND):
            out.append(f"{net} has no pull-down resistor to GND: the translator "
                       f"drives the slot before the FPGA configures")
    for net in RELEASED_NETS:
        if not _has_pull(nl, net, V3V3):
            out.append(f"{net} has no pull-up resistor to {V3V3}: its open-drain "
                       f"output pulls the slot line low at power-up")
    if not _has_pull(nl, RESET_NET, GND):
        out.append(f"{RESET_NET} has no pull-down resistor to GND: /RESET is not held "
                   f"until the FPGA configures")
    jp2 = _pin_nets(nl, "JP2") if "JP2" in nl.parts else {}
    on = [p for p, n in jp2.items() if n == RESET_NET]
    other = next((n for p, n in jp2.items() if p not in on), None) if on else None
    # JP2 may reach +3V3 directly or through one resistor (so the FPGA driving the
    # net low with JP2 fitted does not short +3V3).
    if not on or not (other == V3V3 or any(
            _far_side(nl, r, other) == V3V3 for r in _resistors(nl, other or ""))):
        out.append(f"JP2 does not join {RESET_NET} to {V3V3}: there is no way to "
                   f"release the /RESET hold")
    return out


def _supply_rules(nl: Netlist) -> list[str]:
    out = []
    for value in LV_PARTS:
        for ref in _refs_valued(nl, value):
            for pin, net in sorted(_pin_nets(nl, ref).items()):
                if net in (FUSED_5V, SLOT_5V):
                    out.append(f"{ref} ({value}) pin {pin} is on {net}: a 3.3 V part "
                               f"would drive 5 V into the FPGA")
    for value, pins in TRANSLATOR_SUPPLY.items():
        for ref in _refs_valued(nl, value):
            pn = _pin_nets(nl, ref)
            for pin, rail in sorted(pins.items()):
                if pn.get(pin) != rail:
                    out.append(f"{ref} ({value}) supply pin {pin} is on "
                               f"{pn.get(pin)!r}, not {rail}: A side is the FPGA "
                               f"(3.3 V), B side the slot (fused 5 V)")
    return out


def _data_rules(nl: Netlist) -> list[str]:
    out = []
    for pin in range(42, 50):
        net = nlmod.net_of(nl, "J1", str(pin))
        if net is None:
            out.append(f"J1.{pin} (data) is not on any net")
            continue
        vals = {nl.parts[r].value for r in nlmod.neighbours(nl, net) if r in nl.parts}
        if "74LVC541" not in vals:
            out.append(f"{net} (J1.{pin}) does not reach a 74LVC541: the FPGA could "
                       f"not listen to the real bus")
        if "SN74LVC8T245" not in vals:
            out.append(f"{net} (J1.{pin}) does not reach the SN74LVC8T245: the card "
                       f"could not answer a read")
    return out


def _module_rules(nl: Netlist, module) -> list[str]:
    out = []
    for pin in module.rails["5V"]:
        net = nlmod.net_of(nl, "J2", str(pin))
        if net != MOD_5V:
            out.append(f"J2.{pin} (module 5V) is on {net!r}, not {MOD_5V}")
    for pin in module.rails["GND"]:
        net = nlmod.net_of(nl, "J2", str(pin))
        if net != GND:
            out.append(f"J2.{pin} (module GND) is on {net!r}, not GND")
    # +5V_MOD reaches +5V through one series diode, anode on +5V (Device:D_* pin 2 is
    # the anode, pin 1 the cathode), and through nothing else.
    both = sorted(nlmod.neighbours(nl, MOD_5V) & nlmod.neighbours(nl, FUSED_5V))
    diodes = [r for r in both if r.startswith("D")]
    for ref in both:
        if not ref.startswith("D"):
            out.append(f"{ref} bridges {FUSED_5V} and {MOD_5V}; only a series diode "
                       f"may (a USB cable on the module would back-power the slot)")
    if len(diodes) != 1:
        out.append(f"{MOD_5V} should reach {FUSED_5V} through exactly one series "
                   f"diode, found {len(diodes)}")
    for ref in diodes:
        pn = _pin_nets(nl, ref)
        if pn.get("2") != FUSED_5V or pn.get("1") != MOD_5V:
            out.append(f"{ref} is the wrong way round: its anode (pin 2) must be on "
                       f"{FUSED_5V} and its cathode (pin 1) on {MOD_5V}")
    return out


def problems(nl: Netlist, module=None) -> list[str]:
    """Every design-intent violation in `nl`. `module` is a hw.tools.module.ModuleMap."""
    out = (_rail_rules(nl) + _chain_rules(nl) + _powerup_rules(nl) + _supply_rules(nl)
           + _data_rules(nl))
    if module is not None:
        out += _module_rules(nl, module)
    return out


# --- planted mutations -------------------------------------------------------
#
# Each edits a deep copy of a good netlist so that exactly one rule is violated.

def _move(nl: Netlist, ref: str, pin: str, to: str) -> None:
    for nodes in nl.nets.values():
        nodes.discard((ref, pin))
    nl.nets.setdefault(to, set()).add((ref, pin))


def _drop(nl: Netlist, ref: str) -> None:
    for nodes in nl.nets.values():
        for node in [n for n in nodes if n[0] == ref]:
            nodes.discard(node)
    nl.parts.pop(ref, None)


def _add_part(nl: Netlist, ref: str, value: str, pins: dict[str, str]) -> None:
    nl.parts[ref] = PartInfo(value, "", {})
    for pin, net in pins.items():
        nl.nets.setdefault(net, set()).add((ref, pin))


def _resistor(nl: Netlist, net: str, far: str) -> str:
    for r in _resistors(nl, net):
        if _far_side(nl, r, net) == far:
            return r
    raise LookupError(f"mutation: no resistor between {net} and {far}")


def _pin_of(nl: Netlist, ref: str, net: str) -> str:
    return next(p for r, p in nl.nets[net] if r == ref)


def _drop_resistor(net: str, far: str):
    return lambda nl: _drop(nl, _resistor(nl, net, far))


def _resistor_far_to(net: str, far: str, new_far: str):
    def mutate(nl: Netlist) -> None:
        r = _resistor(nl, net, far)
        _move(nl, r, _pin_of(nl, r, far), new_far)
    return mutate


def _lv_on_5v(value: str):
    def mutate(nl: Netlist) -> None:
        ref = _refs_valued(nl, value)[0]
        pin = next(p for p, n in _pin_nets(nl, ref).items() if n == V3V3)
        _move(nl, ref, pin, FUSED_5V)
    return mutate


def _break_chain(a: str, b: str):
    def mutate(nl: Netlist) -> None:
        na, nb = nlmod.net_of(nl, "J1", a), nlmod.net_of(nl, "J1", b)
        if na == nb:  # joined directly: split the pin off onto its own net
            _move(nl, "J1", b, "UNJOINED")
        else:
            _drop(nl, _resistor(nl, na, nb))
    return mutate


def _fuse_bypassed(nl: Netlist) -> None:
    _add_part(nl, "R_X1", "0", {"1": SLOT_5V, "2": FUSED_5V})


def _load_on_slot_5v(nl: Netlist) -> None:
    ref = _refs_valued(nl, "SN74LVC8T245")[0]
    for pin in ("23", "24"):
        _move(nl, ref, pin, SLOT_5V)


def _gnd_on_second_pin(nl: Netlist) -> None:
    _move(nl, "J1", "27", GND)


def _gnd_off_pin_26(nl: Netlist) -> None:
    _move(nl, "J1", "26", "NOT_GND")


def _load_on_12v(nl: Netlist) -> None:
    net = nlmod.net_of(nl, "J1", "50")
    _add_part(nl, "R_X2", "10k", {"1": net, "2": GND})


def _unbypassed_neg_rail(nl: Netlist) -> None:
    net = nlmod.net_of(nl, "J1", "34")
    for c in [r for r in nlmod.neighbours(nl, net) if r.startswith("C")]:
        _move(nl, c, _pin_of(nl, c, net), "NOT_GND")


def _data_not_listened(nl: Netlist) -> None:
    net = nlmod.net_of(nl, "J1", "45")
    for ref in _refs_valued(nl, "74LVC541"):
        if ref in nlmod.neighbours(nl, net):
            _move(nl, ref, _pin_of(nl, ref, net), "UNLISTENED")


def _data_not_translated(nl: Netlist) -> None:
    net = nlmod.net_of(nl, "J1", "44")
    ref = _refs_valued(nl, "SN74LVC8T245")[0]
    _move(nl, ref, _pin_of(nl, ref, net), "UNTRANSLATED")


def _vcca_on_5v(nl: Netlist) -> None:
    _move(nl, _refs_valued(nl, "SN74LVC8T245")[0], "1", FUSED_5V)


def _vccb_on_3v3(nl: Netlist) -> None:
    _move(nl, _refs_valued(nl, "74ALVC164245")[0], "7", V3V3)


def _jp2_open(nl: Netlist) -> None:
    _move(nl, "JP2", _pin_of(nl, "JP2", RESET_NET), "JP2_OPEN")


MUTATIONS: dict[str, Callable[[Netlist], None]] = {
    "fuse_bypassed": _fuse_bypassed,
    "load_on_slot_5v": _load_on_slot_5v,
    "gnd_on_second_pin": _gnd_on_second_pin,
    "gnd_off_pin_26": _gnd_off_pin_26,
    "load_on_12v": _load_on_12v,
    "unbypassed_neg_rail": _unbypassed_neg_rail,
    "break_int_chain": _break_chain("28", "23"),
    "break_dma_chain": _break_chain("27", "24"),
    "drop_oe_pullup": _drop_resistor("slot_d_oe_n", V3V3),
    "drop_a_oe_pullup": _drop_resistor("slot_a_oe_n", V3V3),
    "dir_pull_to_3v3": _resistor_far_to("slot_d_dir", GND, V3V3),
    "drop_a_dir_pulldown": _drop_resistor("slot_a_dir", GND),
    "drop_release_pullup": _drop_resistor("slot_rdy_o", V3V3),
    "drop_reset_pulldown": _drop_resistor(RESET_NET, GND),
    "jp2_open": _jp2_open,
    "541_on_5v": _lv_on_5v("74LVC541"),
    "2g17_on_5v": _lv_on_5v("74LVC2G17"),
    "1g07_on_5v": _lv_on_5v("74LVC1G07"),
    "translator_vcca_on_5v": _vcca_on_5v,
    "translator_vccb_on_3v3": _vccb_on_3v3,
    "data_not_listened": _data_not_listened,
    "data_not_translated": _data_not_translated,
}


# Module rules need the module map to pick the J2 pins to break.
def _m_5v_pin_off(nl: Netlist, module) -> None:
    _move(nl, "J2", str(module.rails["5V"][0]), FUSED_5V)


def _m_gnd_pin_off(nl: Netlist, module) -> None:
    _move(nl, "J2", str(module.rails["GND"][-1]), "NOT_GND")


def _m_diode_shorted(nl: Netlist, module) -> None:
    for d in [r for r in nlmod.neighbours(nl, MOD_5V) if r.startswith("D")]:
        _drop(nl, d)
    _add_part(nl, "R_X3", "0", {"1": FUSED_5V, "2": MOD_5V})


def _m_second_bridge(nl: Netlist, module) -> None:
    _add_part(nl, "R_X4", "0", {"1": FUSED_5V, "2": MOD_5V})


def _m_diode_reversed(nl: Netlist, module) -> None:
    d = next(r for r in nlmod.neighbours(nl, MOD_5V) if r.startswith("D"))
    _move(nl, d, "1", FUSED_5V)
    _move(nl, d, "2", MOD_5V)


MODULE_MUTATIONS: dict[str, Callable] = {
    "module_5v_pin_on_wrong_net": _m_5v_pin_off,
    "module_gnd_pin_floating": _m_gnd_pin_off,
    "module_diode_shorted": _m_diode_shorted,
    "module_second_bridge": _m_second_bridge,
    "module_diode_reversed": _m_diode_reversed,
}


def selftest(nl: Netlist, module=None) -> list[str]:
    """Failures of the check itself: a clean base that is dirty, or a mutation that
    does not trip anything."""
    fails = [f"base netlist is not clean: {p}" for p in problems(nl, module)]
    cases: dict[str, Callable[[Netlist], None]] = dict(MUTATIONS)
    if module is not None:
        cases.update({k: (lambda n, f=f: f(n, module)) for k, f in MODULE_MUTATIONS.items()})
    for name, mutate in cases.items():
        bad = copy.deepcopy(nl)
        try:
            mutate(bad)
        except (LookupError, StopIteration, KeyError) as e:
            fails.append(f"mutation {name} could not be applied: {e!r}")
            continue
        if not problems(bad, module):
            fails.append(f"mutation {name} produced no problem")
    return fails


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("netlist", type=Path)
    ap.add_argument("--module", type=Path, help="module map TOML; adds the J2 rules")
    ap.add_argument("--selftest", action="store_true",
                    help="plant each mutation and require the check to catch it")
    args = ap.parse_args(argv)
    nl = nlmod.load(args.netlist)
    module = None
    if args.module:
        from hw.tools import module as modmod
        module = modmod.load(args.module)
    if args.selftest:
        fails = selftest(nl, module)
        for f in fails:
            print(f"SELFTEST FAIL: {f}")
        if fails:
            return 1
        n = len(MUTATIONS) + (len(MODULE_MUTATIONS) if module else 0)
        print(f"{n} planted mutations, each caught")
        print("HW INTENT SELFTEST OK")
        return 0
    probs = problems(nl, module)
    for p in probs:
        print(f"INTENT: {p}")
    if probs:
        return 1
    print("HW INTENT OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
