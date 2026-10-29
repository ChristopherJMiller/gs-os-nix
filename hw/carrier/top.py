# SPDX-License-Identifier: CERN-OHL-S-2.0
"""The carrier board: the Apple II slot bus section (hw/src/slot_bus.py), the DDR2
SODIMM socket J2 that holds an iCESugar-Pro module, the carrier's power, the UART
header J4 and the SAFE jumper JP1.

    python3 -m hw.carrier.top --out DIR

writes DIR/carrier.net (the netlist), DIR/carrier.kicad_sch (the schematic), DIR/carrier.lpf
(generated from hw/carrier/pins.toml) and DIR/erc.txt. SKiDL's own side files go to DIR
too (R6).

Power (docs/HARDWARE.md; rulings R13, R14, R16). The module is powered from its five
SODIMM 5V pins and makes its own 3V3, 2V5 and 1V1; it exports no 3V3. Its USB-C VBUS is
tied to those same pins (hw/carrier/icesugar_pro_v1_3.toml, `[notes]`), so:

    +5V_SLOT --F1--> +5V --D1--> +5V_MOD --> J2 5V pins
                                  +5V_MOD --> VR1 (3.3 V LDO) --> +3V3

D1 is the only path between +5V and +5V_MOD: a USB cable on the module cannot push 5 V
into the slot. The LDO runs from +5V_MOD, so the bus section's 3.3 V side is up whenever
the module is, whether the slot or the module's USB supplies it.

Parts (MPN and LCSC checked against JLCPCB's parts search and the LCSC product page,
2026-10-28; datasheets read for the figures below):
  D1  B220A-13-F (Diodes Inc, C110481, SMA, 20 V 2 A Schottky). [B220A] DS13004 Rev 22:
      VF <= 0.50 V at 2 A (25 C); Fig. 1, typical forward characteristic, reads about
      0.37 V at 0.5 A. The 0.45 V ruling is met by the typical curve at 0.5 A: the
      datasheet states no maximum at 0.5 A, only the 0.50 V maximum at 4 times that
      current (a Schottky's VF rises with current, so VF(0.5 A) <= 0.50 V is also
      guaranteed).
  VR1 AP2112K-3.3TRG1 (Diodes Inc, C51118, SOT-23-5). [AP2112] DS39724 Rev 2-2: VIN 2.5
      to 6.0 V, IOUT(max) >= 600 mA, VDROP 125 mV typ / 200 mV max at 300 mA (3.3 V
      option), stable with 1.0 uF X5R/X7R at input and output. Pins (SOT25): 1 VIN, 2 GND,
      3 EN, 5 VOUT (4 is NC). EN is tied to VIN: always on.
  J2  TE Connectivity 1473005-4 (C428482): DDR2 SO-DIMM 200 socket, 0.6 mm pitch.
"""
import argparse
import builtins
import os
from dataclasses import dataclass
from pathlib import Path

from skidl import (KICAD10, POWER, SKIDL, TEMPLATE, Net, Part, Pin, generate_netlist,
                   generate_schematic, reset, set_default_tool)
from skidl.logger import stop_log_file_output

from hw.src import slot_bus as sb
from hw.src.slot_bus import R_10K, C_100N, C_10U, _Build, _nc
from hw.tools import module as modmod
from hw.tools import pins as pinsmod

stop_log_file_output()  # SKiDL's <script>.log/.erc would land in the cwd (R6)

HERE = Path(__file__).resolve().parent
PINS = HERE / "pins.toml"
MODULE = HERE / "icesugar_pro_v1_3.toml"

R_100 = ("100", "0603WAF1000T5E", "C22775")  # basic
C_22U = ("22uF", "CL21A226MAQNNNE", "C45783")  # basic, 25 V X5R, 0805
FP_C0805 = "Capacitor_SMD:C_0805_2012Metric"
FP_HDR3 = "Connector_PinHeader_2.54mm:PinHeader_1x03_P2.54mm_Vertical"
FP_HDR2 = "Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical"

SOCKET = ("1473005-4", "C428482", "gsnix:SODIMM200_DDR2")


class _Carrier(_Build):
    """The bus section's builder, with its own tag prefix: tags are the netlist's stable
    identity for a part, and the bus section already uses t1, t2, ..."""

    def part(self, lib: str, sym: str, **kw) -> Part:
        fields = {k: kw.pop(k) for k in ("MPN", "LCSC", "Assembly") if k in kw}
        self.tags += 1
        p = Part(lib, sym, tag=f"c{self.tags}", **kw)
        p.fields.update(fields)
        return p


@dataclass
class Carrier:
    nets: dict[str, Net]  # every named net of the carrier
    pm: pinsmod.PinMap
    module: modmod.ModuleMap


# A J2 pin's ERC type follows the FPGA port behind it: an FPGA input is a pin the module
# receives on (INPUT), an FPGA output one the module drives (OUTPUT). ERC then sees the
# module as the one driver of `slot_d_dir`, and a second driver there is a conflict.
_PIN_TYPE = {"in": Pin.types.INPUT, "out": Pin.types.OUTPUT, "inout": Pin.types.BIDIR}
_SYM_TYPE = {"PASSIVE": "passive", "INPUT": "input", "OUTPUT": "output",
             "BIDIR": "bidirectional", "NOCONNECT": "no_connect"}


def _symbolize(part: Part, left: list[int], right: list[int]) -> None:
    """Draw a SKiDL-defined connector for the schematic generator: a box with `left`
    pins down its left edge and `right` pins down its right (SKiDL gives such pins no
    geometry, and the schematic generator needs some)."""
    pitch, half_w, length = 2.54, 12.7, 5.08
    rows = max(len(left), len(right))
    top = (rows // 2 + 1) * pitch
    effects = ["effects", ["font", ["size", 1.27, 1.27]]]
    cmds: list = [["rectangle", ["start", -half_w, top], ["end", half_w, top - (rows + 1) * pitch],
                   ["stroke", ["width", 0.254], ["type", "default"]],
                   ["fill", ["type", "background"]]]]
    for side, nums in ((0, left), (180, right)):
        for k, n in enumerate(nums):
            pin = part[n]
            x = -(half_w + length) if side == 0 else half_w + length
            y = top - (k + 1) * pitch
            pin.x, pin.y, pin.length = x, y, length
            pin.orientation, pin.rotation = ("R" if side == 0 else "L"), 0
            cmds.append(["pin", _SYM_TYPE[pin.func.name], "line", ["at", x, y, side],
                         ["length", length], ["name", pin.name or "~", effects],
                         ["number", str(pin.num), effects]])
    part.draw_cmds = {1: cmds}


def _socket(pm: pinsmod.PinMap, mod: modmod.ModuleMap, tag: int) -> Part:
    """J2: the 200 SODIMM contacts, numbered 1..200 as on gsnix:SODIMM200_DDR2 (whose
    solder pegs 'MP' carry no net). A pin's name says what the module puts there."""
    bit_dir = {ball: p.dir for p in pm.ports.values() for _, _, ball in pinsmod.bits(p)}
    rail = {pin: name for name, pins in mod.rails.items() for pin in pins}
    j = Part(tool=SKIDL, name="SODIMM200", ref_prefix="J", dest=TEMPLATE,
             footprint=SOCKET[2], value="iCESugar-Pro")
    for n in range(1, 201):
        if n in rail:
            j += Pin(num=str(n), name=rail[n], func=Pin.types.PASSIVE)
        elif n in mod.balls and mod.balls[n] in bit_dir:
            j += Pin(num=str(n), name=mod.pio[n], func=_PIN_TYPE[bit_dir[mod.balls[n]]])
        elif n in mod.balls:  # a PIO this design does not use
            j += Pin(num=str(n), name=mod.pio[n], func=Pin.types.NOCONNECT)
        else:  # the module leaves it open
            j += Pin(num=str(n), name="NC", func=Pin.types.NOCONNECT)
    j = j(tag=f"c{tag}")
    j.ref = "J2"
    j.fields.update({"MPN": SOCKET[0], "LCSC": SOCKET[1]})
    return j


def build() -> Carrier:
    """Reset SKiDL and build the whole carrier."""
    reset()
    stop_log_file_output()  # reset() opens <script>.log/.erc again (R6)
    set_default_tool(KICAD10)
    pm, mod = pinsmod.load(PINS), modmod.load(MODULE)
    p5v, p3v3, gnd = Net("+5V"), Net("+3V3"), Net("GND")
    p5v_mod = Net("+5V_MOD")
    # ERC wants a source on a rail whose only drivers are passive pins. +3V3 has one
    # (the LDO output); the others take their supply from a connector, a fuse or a
    # diode, all passive. A PWR_FLAG part would say the same, but it has no footprint
    # and would reach the BOM and the PCB import, so the nets are marked instead.
    for n in (p5v, p5v_mod, gnd):
        n.drive = POWER
    bus = sb.slot_bus(p5v, p3v3, gnd)

    b = _Carrier()
    b.nets.update({"+5V": p5v, "+3V3": p3v3, "GND": gnd, "+5V_MOD": p5v_mod})
    b.nets.update(bus.nets)
    b.nets["+5V_SLOT"] = bus.slot_5v

    # --- power: +5V --D1--> +5V_MOD; +5V_MOD -> the module, and -> VR1 -> +3V3 ---
    d1 = b.part("Device", "D_Schottky", ref="D1", value="B220A",
                footprint="Diode_SMD:D_SMA", MPN="B220A-13-F", LCSC="C110481")
    p5v_mod += d1[1]  # Device:D numbering: pin 1 = cathode, pin 2 = anode
    p5v += d1[2]
    b.c(C_22U, p5v_mod, gnd).footprint = FP_C0805
    b.c(C_100N, p5v_mod, gnd)

    vr1 = b.part("Regulator_Linear", "AP2112K-3.3", ref="VR1", value="AP2112K-3.3",
                footprint="Package_TO_SOT_SMD:SOT-23-5", MPN="AP2112K-3.3TRG1",
                LCSC="C51118")
    p5v_mod += vr1["VIN"], vr1["EN"]
    gnd += vr1["GND"]
    p3v3 += vr1["VOUT"]
    nc = _nc()
    nc += vr1[4]
    b.c(C_10U, p5v_mod, gnd)  # input and output: 1 uF minimum, X5R/X7R [AP2112]
    b.c(C_10U, p3v3, gnd)

    # --- the module ---
    b.tags += 1
    j2 = _socket(pm, mod, b.tags)
    for pin in mod.rails["5V"]:
        p5v_mod += j2[pin]
    for pin in mod.rails["GND"]:
        gnd += j2[pin]
    for n in range(1, 201):
        if j2[n].func == Pin.types.NOCONNECT:
            nc += j2[n]
    # FPGA-side nets: the RTL port name (a bus bit is `<port><i>`)
    for port in pm.ports.values():
        if port.on_module:
            continue
        for net, _, ball in pinsmod.bits(port):
            pin = modmod.pin_for_ball(mod, ball)
            b.tie(b.net(net), j2[pin])

    # --- SAFE jumper: safe_n is high (pulled up) unless JP1 is fitted ---
    b.r(R_10K, b.net("safe_n"), p3v3)
    jp1 = b.part("Connector_Generic", "Conn_01x02", ref="JP1", value="SAFE",
                 footprint=FP_HDR2, MPN="PZ254V-11-02P", LCSC="C492401",
                 Assembly="hand")
    b.tie(b.net("safe_n"), jp1[1])
    gnd += jp1[2]

    # --- UART header J4: TX, RX, GND, each line through a 100 ohm series resistor ---
    j4 = b.part("Connector_Generic", "Conn_01x03", ref="J4", value="UART",
                footprint=FP_HDR3, MPN="PZ254V-11-03P", LCSC="C2937625",
                Assembly="hand")
    b.r(R_100, b.net("uart_tx"), b.net("UART_TX"))
    b.r(R_100, b.net("uart_rx"), b.net("UART_RX"))
    b.r(R_10K, b.net("uart_rx"), p3v3)  # idle high with nothing plugged in
    b.tie(b.net("UART_TX"), j4[1])
    b.tie(b.net("UART_RX"), j4[2])
    gnd += j4[3]

    _symbolize(j2, list(range(1, 200, 2)), list(range(2, 201, 2)))
    j1 = next(p for p in builtins.default_circuit.parts if p.ref == "J1")
    _symbolize(j1, list(range(1, 26)), list(range(26, 51)))
    return Carrier(nets=b.nets, pm=pm, module=mod)


def write_outputs(out: Path, schematic: bool = True) -> Path:
    """Build the carrier and write netlist, schematic, LPF and ERC report into `out`."""
    from hw.tools import check_erc  # imported late: it pulls in SKiDL's ERC logger

    out = Path(out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    here = os.getcwd()
    os.chdir(out)  # SKiDL's side files (_sklib.py ...) go to the cwd
    try:
        c = build()
        generate_netlist(file_=str(out / "carrier.net"))
        (out / "carrier.lpf").write_text(pinsmod.to_lpf(c.pm))
        errors, _, _ = check_erc.run_erc(out / "erc.txt")
        if errors:
            raise SystemExit(f"{errors} ERC errors, see {out / 'erc.txt'}")
        if schematic:
            generate_schematic(filepath=str(out), top_name="carrier", mode="labels")
    finally:
        os.chdir(here)
    return out


def main(argv: list[str] | None = None) -> None:
    ap = argparse.ArgumentParser(description="the carrier board's netlist and schematic")
    ap.add_argument("--out", type=Path, required=True, metavar="DIR")
    ap.add_argument("--no-schematic", action="store_true",
                    help="skip carrier.kicad_sch (slow); the netlist, LPF and ERC report stay")
    args = ap.parse_args(argv)
    print(write_outputs(args.out, schematic=not args.no_schematic))


if __name__ == "__main__":
    main()
