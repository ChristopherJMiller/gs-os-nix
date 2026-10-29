# SPDX-License-Identifier: CERN-OHL-S-2.0
"""The Apple II slot bus section: the 50-pin edge connector J1, level translation to the
FPGA's 3.3 V, listen buffers, clock buffers, open-drain outputs, the /RESET hold, the
daisy-chain pass-throughs and the slot-side fuse and bypassing.

One SKiDL subcircuit, reused unchanged on the carrier and the integrated card
(docs/HARDWARE.md "The bus section"). The A side of every translator is the FPGA (3.3 V,
`+3V3`), the B side the slot (5 V, the fused `+5V`). `python3 -m hw.src.slot_bus
--netlist OUT.net` writes the section alone.

Power-up state (the Review Focus the hw-intent check pins): every slot-facing driver is
off until the FPGA configures. Translator /OE is pulled up to +3V3 and DIR down
(slot -> FPGA); the open-drain buffers' inputs are pulled up (released); the /RESET
buffer's input is pulled down, holding /RESET, until the FPGA drives it high or JP2 is
fitted.

Datasheets (TI; each checked for pinout, function table and which supply the control
pins are referenced to):
  [LVC8T245]  SN74LVC8T245 SCES584D, table 7-1: OE=L DIR=L "B data to A bus"; OE=H both
              ports Hi-Z; DIR and OE are referenced to VCCA; pins table 4-1.
  [ALVC164245] SN74ALVC164245 SCAS416Q, table 3: the same function table, per 8-bit
              section; control pins "powered by VCCA"; A port 2.3-3.6 V, B port 3-5.5 V.
              The datasheet numbers bits 1..8 where this design (and the RTL) says 0..7;
              the KiCad symbol numbers them 0..7 too, so xA<k> pairs with xB<k>.
  [LVC1T45]   SN74LVC1T45 SCES515N, table 7-1: DIR=L "B data to A bus"; there is no OE.
  [LVC541A]   SN74LVC541A SCAS298O: OE1, OE2 active low; Y1..Y8 on pins 18..11.
  [LVC2G17]   SN74LVC2G17 SCES381N: 1A=1, GND=2, 2A=3, 2Y=4, VCC=5, 1Y=6 (SC-70-6).
  [LVC1G07]   SN74LVC1G07 SCES296AG: NC=1, A=2, GND=3, Y=4, VCC=5 (SOT-23-5); open-drain,
              Y is Hi-Z for A high; outputs tolerate 5.5 V while Hi-Z.
"""
import argparse
import builtins
import os
from dataclasses import dataclass
from pathlib import Path

from skidl import (KICAD10, POWER, SKIDL, TEMPLATE, Net, Part, Pin, generate_netlist,
                   reset, set_default_tool)
from skidl.logger import stop_log_file_output

# SKiDL opens <script>.log and <script>.erc in the cwd at import, which would leave
# stray files at the repo root (R6); stop that and delete what it just made.
stop_log_file_output()

# --- slot pin names (docs/HARDWARE.md "Slot pinout (IIgs)") --------------------

SLOT_SIGNALS: dict[int, str] = {
    1: "IOSEL_N", 18: "RW", 19: "SYNC_N", 20: "IOSTRB_N", 21: "RDY", 22: "DMA_N",
    23: "INT_OUT", 24: "DMA_OUT", 27: "DMA_IN", 28: "INT_IN", 29: "NMI_N", 30: "IRQ_N",
    31: "RESET_N", 32: "INH_N", 35: "M2B0", 36: "7M", 37: "Q3", 38: "PH1",
    39: "M2SEL_N", 40: "PH0", 41: "DEVSEL_N",
}
SLOT_SIGNALS.update({2 + i: f"A{i}" for i in range(16)})
SLOT_SIGNALS.update({42 + i: f"D{7 - i}" for i in range(8)})  # pin 42 = D7
SLOT_POWER: dict[int, str] = {25: "+5V_SLOT", 26: "GND", 33: "-12V_SLOT",
                              34: "-5V_SLOT", 50: "+12V_SLOT"}

# --- parts (MPN, LCSC checked against JLCPCB's parts search, 2026-10-28) -----

FP_R = "Resistor_SMD:R_0603_1608Metric"
FP_C = "Capacitor_SMD:C_0603_1608Metric"
R_10K = ("10k", "0603WAF1002T5E", "C25804")     # basic
R_4K7 = ("4.7k", "0603WAF4701T5E", "C23162")    # basic
R_1K = ("1k", "0603WAF1001T5E", "C21190")       # basic
R_0 = ("0", "0603WAF0000T5E", "C21189")         # basic, "fitted 0 ohm"
C_100N = ("100nF", "CC0603KRX7R9BB104", "C14663")  # basic, 50 V X7R
C_10U = ("10uF", "CL10A106KP8NNNC", "C19702")      # basic, 10 V X5R

IC = {  # key: (symbol lib, symbol, value, footprint, MPN, LCSC)
    "8T245": ("Logic_LevelTranslator", "SN74LVC8T245", "SN74LVC8T245",
              "Package_SO:TSSOP-24_4.4x7.8mm_P0.65mm", "SN74LVC8T245PWR", "C27643"),
    "164245": ("74xx", "SN74ALVC164245DGG", "74ALVC164245",
               "Package_SO:TSSOP-48_6.1x12.5mm_P0.5mm", "SN74ALVC164245DGGR", "C7128"),
    "1T45": ("Logic_LevelTranslator", "SN74LVC1T45DBV", "SN74LVC1T45",
             "Package_TO_SOT_SMD:SOT-23-6", "SN74LVC1T45DBVR", "C7843"),
    # KiCad has no 74LVC541 symbol; 74AHC541 is the same '541 pinout (OE1 = 1, A1..A8 =
    # 2..9, GND = 10, Y8..Y1 = 11..18, OE2 = 19, VCC = 20). The part is an LVC541A.
    "541": ("74xx", "74AHC541", "74LVC541",
            "Package_SO:TSSOP-20_4.4x6.5mm_P0.65mm", "SN74LVC541APWR", "C113281"),
    "2G17": ("74xGxx", "74LVC2G17", "74LVC2G17",
             "Package_TO_SOT_SMD:SOT-363_SC-70-6", "SN74LVC2G17DCKR", "C50714"),
    "1G07": ("74xGxx", "74LVC1G07", "74LVC1G07",
             "Package_TO_SOT_SMD:SOT-23-5", "SN74LVC1G07DBVR", "C7829"),
}


@dataclass
class SlotBus:
    """The FPGA-side nets of the bus section, by RTL port name (`slot_a0`, `slot_dl7`,
    `slot_d_dir` ...)."""
    nets: dict[str, Net]
    slot_5v: Net  # +5V_SLOT, before the fuse

    def __getitem__(self, name: str) -> Net:
        return self.nets[name]

    def bus(self, port: str, width: int) -> list[Net]:
        return [self.nets[f"{port}{i}"] for i in range(width)]


class _Build:
    """Net registry and part helpers. Net names are the netlist's identity, so every
    net is made through here and shared by name."""

    def __init__(self) -> None:
        self.nets: dict[str, Net] = {}
        self.tags = 0

    def part(self, lib: str, sym: str, **kw) -> Part:
        """A library part. MPN, LCSC and Assembly become netlist fields, and every part
        gets a counter tag, so the netlist is the same on every run."""
        fields = {k: kw.pop(k) for k in ("MPN", "LCSC", "Assembly") if k in kw}
        self.tags += 1
        p = Part(lib, sym, tag=f"t{self.tags}", **kw)
        p.fields.update(fields)
        return p

    def net(self, name: str) -> Net:
        if name not in self.nets:
            self.nets[name] = Net(name)
        return self.nets[name]

    def tie(self, net: Net, *pins) -> None:
        for p in pins:
            net += p

    def r(self, spec, a: Net, b: Net):
        value, mpn, lcsc = spec
        r = self.part("Device", "R", value=value, footprint=FP_R, MPN=mpn, LCSC=lcsc)
        a += r[1]
        b += r[2]
        return r

    def c(self, spec, rail: Net, gnd: Net):
        value, mpn, lcsc = spec
        c = self.part("Device", "C", value=value, footprint=FP_C, MPN=mpn, LCSC=lcsc)
        rail += c[1]
        gnd += c[2]
        return c

    def ic(self, key: str):
        lib, sym, value, fp, mpn, lcsc = IC[key]
        return self.part(lib, sym, value=value, footprint=fp, MPN=mpn, LCSC=lcsc)


def _nc() -> Net:
    """SKiDL's no-connect net: for pins left open on purpose (ERC then stays quiet).
    SKiDL publishes the default circuit as a builtin."""
    return builtins.default_circuit.NC


def _named(part, name: str):
    """Every pin of `part` whose symbol name is exactly `name`. Connecting by name keeps
    a symbol's pin order from silently mis-wiring a part."""
    pins = [p for p in part.pins if p.name == name]
    if not pins:
        raise KeyError(f"{part.name} has no pin named {name!r}: "
                       f"{sorted({p.name for p in part.pins})}")
    return pins


def _numbered(part, num: int, func: str):
    """A pin by number, for symbols whose pins carry no names (the single and dual
    gates); checks the symbol's pin type so a wrong number cannot pass."""
    pin = part[num]
    assert pin.func.name.startswith(func), f"{part.name} pin {num} is {pin.func.name}"
    return pin


def _slot_connector(tag: int) -> Part:
    """J1: the board's own edge fingers, copper rather than a purchased part. Pin
    numbers are slot pin numbers (gsnix:AppleII_Slot_Edge pads)."""
    names = {**{n: f"/{s}" if s.endswith("_N") else s for n, s in SLOT_SIGNALS.items()},
             **SLOT_POWER}
    j = Part(tool=SKIDL, name="AppleII_Slot_Edge", ref_prefix="J", dest=TEMPLATE,
             footprint="gsnix:AppleII_Slot_Edge", value="Apple II slot")
    for n in range(1, 51):
        j += Pin(num=str(n), name=names[n], func=Pin.types.PASSIVE)
    j = j(tag=f"t{tag}")
    j.ref = "J1"
    j.fields["Fab"] = "pcb"  # board copper: no LCSC, left out of the BOM and CPL
    return j


def slot_bus(p5v: Net, p3v3: Net, gnd: Net) -> SlotBus:
    """Build the bus section. `p5v` is the fused 5 V (after F1), `p3v3` the FPGA-side
    3.3 V, `gnd` ground. Returns the FPGA-side nets."""
    b = _Build()
    for n, net in (("+5V", p5v), ("+3V3", p3v3), ("GND", gnd)):
        assert net.name == n, f"slot_bus: expected net {n}, got {net.name}"
        b.nets[n] = net
    n3, n5, g = p3v3, p5v, gnd

    # --- J1 and the slot nets ---
    b.tags += 1
    j1 = _slot_connector(b.tags)
    for num, name in SLOT_POWER.items():
        b.tie(b.net(name), j1[num])
    for num, name in SLOT_SIGNALS.items():
        b.tie(b.net(f"SLOT_{name}"), j1[num])

    # --- power: slot 5 V -> fuse -> +5V. Slot 5 V reaches nothing else. ---
    slot5 = b.net("+5V_SLOT")
    f1 = b.part("Device", "Polyfuse", ref="F1", value="500mA",
              footprint="Fuse:Fuse_1812_4532Metric", MPN="MF-MSMF050-2", LCSC="C17313")
    slot5 += f1[1]
    n5 += f1[2]
    b.c(C_100N, slot5, g)
    b.c(C_10U, n5, g)
    b.c(C_10U, n3, g)
    for rail in ("+12V_SLOT", "-5V_SLOT", "-12V_SLOT"):
        b.c(C_100N, b.net(rail), g)

    def fpga(name: str) -> Net:
        return b.net(name)

    # --- data: SN74LVC8T245. A = FPGA, B = slot. DIR low = B->A (slot -> FPGA). ---
    d = b.ic("8T245")
    b.tie(n3, *_named(d, "V_{CCA}"))
    b.tie(n5, *_named(d, "V_{CCB}"))  # fused +5V, never +5V_SLOT
    b.tie(g, *_named(d, "GND"))
    b.tie(fpga("slot_d_dir"), *_named(d, "DIR"))
    b.tie(fpga("slot_d_oe_n"), *_named(d, "~{OE}"))
    for i in range(8):
        b.tie(fpga(f"slot_d{i}"), *_named(d, f"A{i + 1}"))
        b.tie(b.net(f"SLOT_D{i}"), *_named(d, f"B{i + 1}"))
    b.c(C_100N, n3, g)  # VCCA
    b.c(C_100N, n5, g)  # VCCB (pins 23 and 24 are adjacent: one capacitor)

    # --- address: 74ALVC164245, A0-A15. A = FPGA, B = slot. Either DIR low = B->A. ---
    a = b.ic("164245")
    b.tie(n3, *_named(a, "V_{CC(A)}"))
    b.tie(n5, *_named(a, "V_{CC(B)}"))
    b.tie(g, *_named(a, "GND"))
    b.tie(fpga("slot_a_dir"), *_named(a, "1DIR"), *_named(a, "2DIR"))
    b.tie(fpga("slot_a_oe_n"), *_named(a, "1~{OE}"), *_named(a, "2~{OE}"))
    for i in range(16):
        grp = 1 + i // 8
        bit = i % 8
        b.tie(fpga(f"slot_a{i}"), *_named(a, f"{grp}A{bit}"))
        b.tie(b.net(f"SLOT_A{i}"), *_named(a, f"{grp}B{bit}"))
    for _ in range(2):  # VCCA pins 31 and 42
        b.c(C_100N, n3, g)
    for _ in range(2):  # VCCB pins 7 and 18
        b.c(C_100N, n5, g)

    # --- R/W: SN74LVC1T45, DIR shared with the address bus. ---
    rw = b.ic("1T45")
    b.tie(n3, *_named(rw, "VCCA"))
    b.tie(n5, *_named(rw, "VCCB"))
    b.tie(g, *_named(rw, "GND"))
    b.tie(fpga("slot_a_dir"), *_named(rw, "DIR"))
    b.tie(fpga("slot_rw"), *_named(rw, "A"))
    b.tie(b.net("SLOT_RW"), *_named(rw, "B"))
    b.c(C_100N, n3, g)
    b.c(C_100N, n5, g)

    # --- listen-only buffers, 74LVC541 on +3V3, always enabled (OE1, OE2 to GND). ---
    def lvc541(inputs: list[tuple[str, str]]) -> None:
        u = b.ic("541")
        b.tie(n3, *_named(u, "VCC"))
        b.tie(g, *_named(u, "GND"), *_named(u, "G1"), *_named(u, "G2"))
        for i in range(8):
            if i < len(inputs):
                src, dst = inputs[i]
                b.tie(b.net(src), *_named(u, f"A{i}"))
                b.tie(fpga(dst), *_named(u, f"Y{i}"))
            else:  # an unused input must not float; its output is left open
                b.tie(g, *_named(u, f"A{i}"))
                b.tie(_nc(), *_named(u, f"Y{i}"))
        b.c(C_100N, n3, g)

    lvc541([(f"SLOT_D{i}", f"slot_dl{i}") for i in range(8)])
    lvc541([("SLOT_DEVSEL_N", "slot_devsel_n"), ("SLOT_IOSEL_N", "slot_iosel_n"),
            ("SLOT_IOSTRB_N", "slot_iostrb_n"), ("SLOT_M2SEL_N", "slot_m2sel_n"),
            ("SLOT_M2B0", "slot_m2b0"), ("SLOT_RESET_N", "slot_reset_n"),
            ("SLOT_SYNC_N", "slot_sync_n")])

    # --- clocks: 74LVC2G17 Schmitt buffers, each output through a 0 ohm series
    #     resistor (a footprint for a damping resistor if an edge rings). ---
    for pair in (("ph0", "ph1"), ("q3", "7m")):
        u = b.ic("2G17")
        b.tie(n3, _numbered(u, 5, "PWR"))
        b.tie(g, _numbered(u, 2, "PWR"))
        for (ain, yout), name in zip(((1, 6), (3, 4)), pair):
            b.tie(b.net(f"SLOT_{name.upper()}"), _numbered(u, ain, "INP"))
            buf = b.net(f"CLK_{name.upper()}_BUF")
            b.tie(buf, _numbered(u, yout, "OUT"))
            b.r(R_0, buf, fpga(f"slot_{name}"))
        b.c(C_100N, n3, g)

    # --- open-drain outputs: 74LVC1G07. Input high = released. ---
    for port, slot in (("slot_irq_n_o", "SLOT_IRQ_N"), ("slot_nmi_n_o", "SLOT_NMI_N"),
                       ("slot_inh_n_o", "SLOT_INH_N"), ("slot_dma_n_o", "SLOT_DMA_N"),
                       ("slot_rdy_o", "SLOT_RDY"), ("slot_reset_n_o", "SLOT_RESET_N")):
        u = b.ic("1G07")
        b.tie(n3, _numbered(u, 5, "PWR"))
        b.tie(g, _numbered(u, 3, "PWR"))
        b.tie(fpga(port), _numbered(u, 2, "INP"))
        b.tie(b.net(slot), _numbered(u, 4, "OPE"))
        b.tie(_nc(), u[1])
        b.c(C_100N, n3, g)
        if port != "slot_reset_n_o":
            b.r(R_10K, fpga(port), n3)  # released until the gateware is armed
    # /RESET is held low until the FPGA reports DONE and drives the line high.
    # JP2 (through 1k so the FPGA driving low with JP2 fitted does not short +3V3)
    # pulls the line high, releasing the hold.
    b.r(R_4K7, fpga("slot_reset_n_o"), g)
    jp2 = b.part("Connector_Generic", "Conn_01x02", ref="JP2", value="RESET hold off",
               footprint="Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical",
               MPN="PZ254V-11-02P", LCSC="C492401", Assembly="hand")
    jp2_net = b.net("JP2_3V3")
    b.tie(fpga("slot_reset_n_o"), jp2[1])
    b.tie(jp2_net, jp2[2])
    b.r(R_1K, jp2_net, n3)

    # --- power-up: translators off, slot -> FPGA. ---
    b.r(R_10K, fpga("slot_d_oe_n"), n3)
    b.r(R_10K, fpga("slot_a_oe_n"), n3)
    b.r(R_10K, fpga("slot_d_dir"), g)
    b.r(R_10K, fpga("slot_a_dir"), g)

    # --- daisy chains: an idle card passes INT IN -> INT OUT, DMA IN -> DMA OUT. ---
    b.r(R_0, b.net("SLOT_INT_IN"), b.net("SLOT_INT_OUT"))
    b.r(R_0, b.net("SLOT_DMA_IN"), b.net("SLOT_DMA_OUT"))

    fpga_nets = {n: net for n, net in b.nets.items()
                 if n.startswith("slot_")}
    return SlotBus(nets=fpga_nets, slot_5v=slot5)


def build_netlist(out: Path) -> Path:
    """Build the bus section alone and write its netlist to `out`. SKiDL drops its
    .erc/.log/_sklib.py files into the cwd, so run it in OUT's directory."""
    out = Path(out).resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    here = os.getcwd()
    os.chdir(out.parent)
    try:
        reset()
        set_default_tool(KICAD10)
        p5v, p3v3, gnd = Net("+5V"), Net("+3V3"), Net("GND")
        for n in (p5v, p3v3, gnd):  # stand-ins for the regulators and the slot ground
            n.drive = POWER
        slot_bus(p5v, p3v3, gnd)
        generate_netlist(file_=str(out))
    finally:
        os.chdir(here)
    return out


def main(argv: list[str] | None = None) -> None:
    ap = argparse.ArgumentParser(description="the Apple II slot bus section, alone")
    ap.add_argument("--netlist", type=Path, required=True, metavar="OUT.net")
    args = ap.parse_args(argv)
    print(build_netlist(args.netlist))


if __name__ == "__main__":
    main()
