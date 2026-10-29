# Hardware: the physical card

**Status: the carrier is designed and its fab package built (2026-10-28); the owner's
review and the order are pending, and nothing is fabricated.** This document is the plan for
turning `rtl/a2nix_card.v` into a card that plugs into a real Apple IIgs, and for how an
agent designs that card. It records the owner's decisions, the facts they rest on, and
what is left to whoever builds it. As hardware lands, settled entries move to
[DECISIONS.md](DECISIONS.md) and verified facts to [REFERENCES.md](REFERENCES.md).

## Why

The software side (VM, compiler, card RTL in MAME) is advancing; the hardware side has
nothing. The card exists only as Verilog that MAME verilates. This plan is the first
step toward the same bitstream running in a real slot.

## Goals, non-goals, anti-goals

**Goals**

1. **An open hardware release.** Anyone can fab the card from the repo's files at a
   reasonable cost, with open tools end to end: yosys/nextpnr/trellis for the FPGA,
   KiCad for the board. *(owner)*
2. **The same core RTL as MAME.** `a2nix_card` is not forked for hardware; board-level
   logic wraps it. The one change the hardware needs in the core -- a memory wait input
   (HW-7) -- is made once and merged, so MAME and the card keep running the same logic,
   at the same clock (HW-8).
3. **Built-in bus capture.** The card can record Apple II slot traffic, because the
   first real machine will disagree with MAME's idealised timing and the card must be
   able to show how. *(owner)*
4. **An agent can design it.** The board is source code plus checks, gated the way the
   rest of the repo is: every check proven able to fail (D14).

**Non-goals (for now)**

- The IIgs memory expansion slot. Considered and set aside by the owner: it is fast
  (no 1 MHz throttle), but it only looks like DRAM -- no I/O space, no IRQ, no DMA, an
  8 MB ceiling, and DRAM-style RAS/CAS timing to emulate. Facts in "The memory slot"
  below.
- Onboard USB on rev A. An external JTAG adapter is acceptable *(owner)*. (The carrier
  uses its module's USB, HW-13.)
- Gowin parts, until Apicula's miscompute bugs close (HW-1).
- Reflashing the card from the IIgs, and any change to [CARD-ISA.md](CARD-ISA.md).
  Both are possible later; both belong to the software side.
- DMA. The address shifter can drive the bus so DMA stays possible, but nothing uses it.
- Apple II models other than the IIgs.

**Anti-goals** -- it fails even if it works, if:

- The card makes a decision or does I/O (D2). Capture is passive: it may only drive a
  bus line when explicitly armed to.
- The hardware forks the core RTL, so MAME and the real card run different logic.
- A board passes its checks but nobody could reproduce it from the repo (hand edits
  outside the source, unpinned tools, parts with no supplier).

## Decisions

The owner made HW-1, HW-2, HW-3, HW-7, HW-8, HW-9 and HW-10. The rest are the
designer's calls, each open to the owner's veto.

### HW-1 -- ECP5 LFE5U-25F (BGA256) with 32 MB SDRAM; D10 stands *(owner)*

Only ECP5 combines a mature open flow, a hardware fallback bitstream, and parts in
stock at hobby prices (survey 2026-10-27):

| Option | Package | Open flow | FPGA + RAM | Verdict |
|---|---|---|---|---|
| **LFE5U-25F-6BG256C** + W9825G6KH-6 | 256-ball BGA, 0.8 mm | Trellis: mature; per-speed-grade timing | ~$21; LCSC 158 FPGAs, 2875 SDRAM | **chosen** |
| LFE5U-25F-6TG144C | TQFP-144 | same | $33; **1** at LCSC, 40-week lead | supply rules it out; ~93 of 98 I/O used |
| Gowin GW2AR-18 | QFN-88, 8 MB SDRAM in package | Apicula: **carry chains compute wrong on this die** ([apicula#514], [#541]); no hardware fallback | $44, LCSC only | not yet |
| GateMate CCGM1A1 | BGA-324 | vendor flow fully open | $27 | fit untested; I/O ≤ 2.5 V |
| iCE40 | -- | mature | -- | too small: ≤7.7k LUT vs 11.2k measured |

The CAM comparators, `alu64` and the SHA adders are all carry-chain logic, which is
why the Gowin bugs are disqualifying rather than cosmetic. Revisit if they close: a
QFN part with SDRAM in the package would remove the SDRAM routing entirely.

Byte Hamr (Dec 2025) is an ECP5-85F Apple IIe card with 64 MB SDRAM built with
yosys/nextpnr -- the route is proven on this bus.

### HW-2 -- SKiDL + KiCad 10 + Freerouting *(owner)*

The schematic is Python (SKiDL, MIT), which emits a netlist and an editable
`.kicad_sch`. KiCad 10 does layout, ERC/DRC and fab outputs. Freerouting routes.

- KiCad 10.0.6 runs fully headless in a `sandbox = true` Nix build against this repo's
  pinned nixpkgs: ERC/DRC with JSON output, gerbers, renders, and a
  DSN → Freerouting → SES round trip through the `pcbnew` Python module all worked
  (tested 2026-10-27).
- **KiCad 11 removes the SWIG `pcbnew` bindings** that the routing round trip uses; its
  headless IPC API replaces them. Pin KiCad 10 until the move is planned.
- Rejected: Diode's `pcb`/Zener (agent-native and promising, but v0.4 from one company
  with features on its servers); tscircuit (v0.0.x, daily churn, one-way KiCad
  export); atopile (pre-release churn); a hand-drawn KiCad schematic (an agent cannot
  draw one well).

### HW-3 -- A 2-layer bus carrier comes before the integrated card *(owner)*

The toolchain is proven on a small real board: an Apple II slot card carrying the
**bus section** (fingers, level shifters, capture buffer, reset hold, power, debug
headers) and a DDR2 SODIMM-200 socket for an **iCESugar-Pro** module -- the same
LFE5U-25F with 32 MB SDRAM and 106 free I/O. The bus section is one SKiDL subcircuit,
reused unchanged on the integrated card, so the carrier tests the part of the design
that is new while the module covers the FPGA, SDRAM and power that are already proven.

The carrier is designed to the module's current published pinout
([wuxx/icesugar-pro](https://github.com/wuxx/icesugar-pro)). The owner buys one before
the carrier is ordered, and its revision is checked against the pinout then.

Risks: the iCESugar-Pro's stock is unconfirmed (Tindie, $60), and it has schematics but
no hardware licence. It is a development fixture, not part of the release. Fallback:
Icepi Zero (OSHWA-certified, in stock, $79), but with 28 I/O the bus would need
multiplexing, so the bus section would not carry over unchanged.

### HW-4 -- Fab: JLCPCB by default, PCBWay for hard gold

JLCPCB assembles BGAs (X-ray inspected), stocks the parts through LCSC, and offers
beveled gold fingers -- but **ENIG only**. ENIG wears through after a few insertions
(Big Mess o' Wires), which matters little for a card that stays seated. PCBWay offers
hard gold and 20-60° bevels. The fab outputs target JLCPCB; the doc for PCBWay is a
note, not a second set of files.

### HW-5 -- Hardware work happens in its own worktree

The software agent works on `main` with uncommitted changes, including `flake.nix`.
Hardware work lives on its own branch in its own git worktree, and reaches `main`
through the owner. Edits to shared files (`flake.nix`, `README.md`) are kept small and
coordinated.

### HW-6 -- Hardware tools live in their own dev shell

KiCad's closure is 2.5 GiB. `nix develop .#hw` carries KiCad 10, SKiDL, Freerouting,
KiKit and Interactive HTML BOM, so the default shell does not grow. The `hw-*` checks
still run under `nix flake check` -- one gate -- since cache.nixos.org has the binaries.

Packaging gaps in the pinned nixpkgs: SKiDL is 2.2.1 (3.0.0 is current and writes KiCad
10 schematics); trellis is from 2025-01-30, before `ecpmulti --golden` (2025-03-29);
openFPGALoader is 1.1.1 (1.2.0 current).

### HW-7 -- The core's memory port gets a wait input *(owner)*

The heap port is "one-cycle, BRAM-like" ([HEAP.md](HEAP.md)): read data is valid the
cycle after the address, and nothing can say "not yet". Real SDRAM, and D12's cache in
front of it, cannot always answer in one cycle, so the core gets a **`mem_ready`**
input: while it is low, the core holds its memory request and waits. Rejected: gating
the core's clock from outside with the ECP5's DCCA clock enable. It needs no core
change, but it hides the stall from the core and makes the bus-facing `c0n_*` strobes
wait on a stopped clock.

- The change is made in the hardware worktree (HW-5) and merged into `main` through the
  owner, coordinated with the software agent, which owns the core.
- MAME (`mame/a2nix.cpp`) and the Verilator harness (`sim/card_main.cpp`) tie
  `mem_ready` high, so their behaviour and cycle counts do not change.
- A new test drives `mem_ready` with random wait states and requires the same results
  as with none. Its mutation (the core ignoring `mem_ready`) must fail it (D14).
- [HEAP.md](HEAP.md)'s port description is updated in the same change.

### HW-8 -- The card runs at the clock it meets, and MAME follows *(owner)*

Measured 2026-10-27, `a2nix_card` alone (out of context, no I/O), yosys's default
`synth_ecp5` flow, nextpnr seed 1, LFE5U-25F-6BG256C: **14,680 LUT4 (60%), 6,180 FF,
43.0 MHz after routing**, against the 50 MHz MAME models. The slowest path is LOOKUP:
`pslot[0]` (`cam_key`) → keycam compare and index → the `lk_hit_index` add →
`pslot[1]` and `mem_addr`, all in one clock. Board logic will only make it slower.

- Target **~40 MHz**. The `hw-timing` gate on each board top sets the number actually
  used.
- **One constant** holds the card clock, read by both the PLL configuration and MAME's
  `CARD_CLOCK_HZ` (`mame/a2nix.cpp`), so the two cannot disagree. Changing MAME's clock
  is the software side's change: `checks.verb-economics` and the speed figures are
  re-measured at the new clock. It is made in the hardware worktree and merged like
  HW-7.
- Rejected: pipelining LOOKUP in the core to hold 50 MHz -- a core change whose margin
  is uncertain once board logic is added, and still possible later if verb economics
  demand it. Also rejected: the -8 speed grade -- the iCESugar-Pro is -6, and -8
  price and stock are unchecked.
- ARCHITECTURE.md's 11.2k LUT4 is the `-noabc9` count `just area` reports; nextpnr
  places the default flow's 14.7k. Budget against the larger number.

### HW-9 -- Rev A is designed now, and ordered only after the carrier has run in a real IIgs *(owner)*

The open questions below feed the bus section, which rev A reuses unchanged. So rev A's
design proceeds to order-ready alongside the carrier, but rev A is not ordered until
the carrier has run in a real IIgs and what it found is folded back into the bus
section.

Carrier bring-up is staged so that a fault cannot reach the machine:

1. Bench: power and programming only, no IIgs (on the carrier, over the module's own
   USB; HW-13).
2. In the IIgs, a **listen-only** bitstream: every data and address buffer disabled
   toward the slot, every open-drain line released, capture recording.
3. Read-only responses: the `ID` register at `$C0n1`.
4. The full core.

Each stage's capture is kept as the record of what the real bus did. The card's own
capture is the main instrument.

### HW-10 -- Board files are CERN-OHL-S-2.0 *(owner)*

`hw/` carries its own licence, **CERN-OHL-S-2.0**, the strongly reciprocal hardware
licence: anyone who ships a modified board publishes its sources. The RTL (including
`rtl/board/`) and all code stay GPL-3.0-or-later. Anything vendored into `hw/` --
footprints, symbols, templates -- must have a compatible licence, and is recorded in
[SOURCES.md](SOURCES.md)'s style.

### HW-11 -- What gets the 25F's room

With the core at 60% of the part:

- **D12's cache is an optimisation** behind `mem_ready`, added once measurement shows it
  pays, as verbs must (`checks.verb-economics`). The first bitstream may stall on every
  SDRAM access; it is correct either way.
- **Always built in:** capture's cycle recorder, the margin monitor, and the JTAG and
  UART readout. The edge recorder and the full trigger sequencer may move to a separate
  debug bitstream if area or timing demands. The flash has room for it.
- **A drop-in LFE5U-45F**, which also comes in caBGA256, is kept possible where
  Lattice's pinouts allow; `hw-pinmap` checks the map against both parts' pinout files.

### HW-12 -- The heap stays 1 MB on rev A; the SDRAM map leaves it room

The core's heap is fixed (`SPACE_SIZE`, 512 KB a side, matching MAME's `HEAP_BYTES`).
Rev A runs the same 1 MB, so the card and MAME behave alike. The SDRAM map reserves its
first 16 MB for the heap and puts capture's ring above it. Growing the heap is the
software side's decision.

### HW-13 -- The carrier has no JTAG header; it is programmed over the module's USB

The iCESugar-Pro v1.3 routes JTAG (TCK, TMS, TDI, TDO) and its serial pins (B9, A9)
only to its on-module iCELink MCU and its own header J1. None reaches the SODIMM
**V** [module schematic, `hw/carrier/icesugar_pro_v1_3.toml`]. The carrier therefore
programs and reads the console through the module's USB (iCELink is CMSIS-DAP plus a
VCOM port: `openFPGALoader -c cmsisdap`). It keeps a 1×3 UART header **J4** (100 Ω in
series per line) and the SAFE jumper **JP1** on spare PIOs (`uart_tx`, `uart_rx`,
`safe_n`). Rejected: a Cortex-10 header on the carrier, which would connect to nothing.
Rev A keeps its JTAG header as designed.

Readout on the carrier: the JTAG user-register readout (ER1 on the ECP5 `JTAGG`
primitive, see Capture) goes through the module's iCELink, a CMSIS-DAP JTAG adapter
and the same one that programs the module, so the host tool needs a CMSIS-DAP-capable
JTAG driver (openocd's `cmsis-dap`). J4's UART carries the filtered stream. **I**
(inferred, not yet exercised; plan 3 builds it). On rev A both go through the Cortex-10
header and an external adapter.

### HW-14 -- The carrier powers the module through a diode, and makes only its own 3.3 V

The SODIMM carries only +5 V (pins 2, 4, 6, 8, 10, 12) and GND; the module makes its
own 3V3, 2V5 and 1V1 **V** [module schematic]. The module ties its USB VBUS straight to
those 5 V pins, with no diode, so a USB cable would back-power the slot. The carrier
feeds the module through **D1**, a Schottky (B220A-13-F, LCSC C110481): `+5V` →
`+5V_MOD`. The carrier's `+3V3` comes from **VR1**, an AP2112K-3.3 (C51118) fed from
`+5V_MOD`, so the bus section's 3.3 V side tracks the module whether the slot or USB
supplies it. Rejected: a direct connection (the back-power above). The datasheet gives
D1's Vf as 0.50 V max at 2 A and states none at 0.5 A; the module's SY8008 bucks have
headroom either way **I**.

### HW-15 -- The gates as built: each reads what it can prove, and proves it can fail

Departures from the first design, each because the first idea read the wrong artefact:

- `hw-erc` runs SKiDL's `ERC()` on the netlist. Rejected: `kicad-cli` ERC on the
  generated schematic, which the labels-only schematic (a human artefact) cannot
  support.
- `hw-drc` is KiCad DRC plus our own netlist ↔ board parity (`check_drc.py`).
  Rejected: `--schematic-parity`, for the same reason.
- `hw-fab` checks the package's contents on mutated copies. Rejected: byte identity,
  because KiCad stamps dates into gerbers.
- `hw-intent` states what ERC and DRC cannot see (power-up state, daisy chains, rails,
  module power) as rules on the netlist, each with a planted mutation. `hw-tests`
  runs pytest over `hw/tests`.

`kicad-cli` and KiKit run only on temporary copies of the board, so a gate never edits
the committed one. `hw-timing` and `hw-rtl` are not built yet (plan 3).

## Facts the design rests on

**V** = verified at the source given; **I** = inferred, check before relying on it.
Timing comes from Apple's **APDA draft Hardware Reference of 21 Nov 1986** [HWR] and
Tech Note #68 [TN68]; TN #30 lists the corrections to the published editions, and its
only slot-chapter one is the DMA bank register ($C037).

### Slot pinout (IIgs)

Pins 1-25 on the component side, pin 1 toward the front. "Dir" is from the card.

| Pin | Signal | Dir | Notes |
|---|---|---|---|
| 1 | /IOSEL | in | $CnXX; low only while PH0 high |
| 2-17 | A0-A15 | in (out in DMA) | slot side buffered by 74HCT245s **V** [HWR 5-7] |
| 18 | R/W | in (out in DMA) | |
| 19 | /SYNC | in | slot 7 only |
| 20 | /IOSTRB | in | $C800-$CFFF |
| 21, 22 | RDY, /DMA | OC out | 4.7 k / 3.3 k pull-ups |
| 23, 24 | INT OUT, DMA OUT | out | NC on slot 7 |
| 25, 26 | +5 V, GND | | **the only ground pin** **V** [TN68] |
| 27, 28 | DMA IN, INT IN | in | NC on slot 1 |
| 29, 30, 32 | /NMI, /IRQ, /INH | OC out | /INH honoured in slow mode only |
| 31 | /RESET | in / OC out | |
| 33, 34, 50 | -12 V, -5 V, +12 V | | |
| 35 | M2B0 (CREF on slot 7) | in | slot 3 only on ROM 00/01; slots 1-6 on ROM 3 |
| 36, 37 | 7M, Q3 | in | 7.16 MHz; Q3 high ≥ 270 ns, low ≥ 200 ns |
| 38, 40 | PH1, PH0 | in | the 1 MHz Mega II clock, **not** the CPU's PH2 |
| 39 | /M2SEL | in | replaces µPSYNC; **low during a synchronised slow cycle** |
| 41 | /DEVSEL | in | $C0(n+8)X |
| 42-49 | D7-D0 | bidir | pin 42 = D7 |

- The slot register **$C02D** enables a slot's ROM space *and* its $C0nX I/O space; a
  slot left at "Internal" never sends /DEVSEL to the card **V** [HWR 3-10].
- Daisy-chain direction: the pin figure and the chapter's summary conflict. An idle
  card passes INT IN → INT OUT and DMA IN → DMA OUT either way **V** [HWR 5-2, 5-13].

### Slow-cycle timing at the slot pins [HWR fig 5-5, TN68]

| Parameter | Min (ns) | Max (ns) |
|---|---:|---:|
| PH0 high / low time | 480 / 480 | |
| /DEVSEL, /IOSEL, /IOSTRB low after PH0↑ | | 15 |
| Address, R/W valid after PH0↓ | | 100 |
| Write data valid after PH0↑ / hold after PH0↓ | -- / 30 | 30 / -- |
| **Read data setup before PH0↓ / hold after** | **140 / 10** | |
| /IRQ, /NMI, RDY setup before PH0↓ | 140 [HWR]; 60 [TN68] | |

- Read budget from /DEVSEL to valid data: 480 - 15 - 140 ≈ **325 ns**, less shifter
  delay **I**. Drive read data from the **first 7M↑ after PH0↑**; release it **0-30 ns
  after PH0↓**, since the IIgs's own buffers turn around within 30 ns of each PH0 edge
  **V** [TN68 fig 3].
- Write latch window: about PH0↑ + 30 ns to PH0↓ + 30 ns **I**.
- Change RDY only while PH0 is high with /M2SEL or a select low **V** [TN68].
- Consecutive slow cycles take 980 ns; an **isolated slow cycle averages ≈ 1.5 µs**
  (synchronisation wait); a fast cycle is 350 ns **V** [TN68].
- Modern practice: a2pico latches A0-A11 on the select's fall and write data ~300 ns
  later; A2FPGA samples data ~278 ns after PH0↑, only when /M2SEL is low, and has an
  open bug where "some IIgs systems sample garbage data" **V** [A2FPGA].

### What a slot can see (this bounds capture)

| Cycle | /M2SEL | Address on slot | Data on slot |
|---|---|---|---|
| Mega II: I/O, slot spaces, banks $E0/$E1, shadowed video writes | low | valid all cycle | valid while PH0 high |
| Fast: fast RAM, expansion RAM, ROM, FPI registers | high | **"not valid during the entire PH0 cycle"** | not the CPU's **I** |
| PH0 low, every cycle | | | video data |

The bank byte never reaches a slot. **A slot-resident analyzer sees the card's own
cycles and other slow traffic, but cannot follow fast-RAM execution** -- IIgs
accelerators take over at the CPU socket for that reason **V** [HWR 5-10, TN68].

### Power

- The slot budget is a **total for all slots**: 500 mA at +5 V, 250 mA at +12 V,
  200 mA each at -5 V and -12 V, through **one +5 V pin and one GND pin** per slot
  **V** [HWR 5-13, TN68]. TN68: single cards over budget "generally work"; several
  overheat the case.
- Power-supply ratings conflict between sources (+5 V 2.5 A in the draft, 4.0 A in the
  FAQ); a moderately loaded IIgs draws ~3 A at +5 V **V** [FAQ-PWR]. A rail near
  4.75 V gives marginal sampling; A2FPGA recommends ~5.05 V **V** [A2FPGA].

### Mechanical

- Slots 4-7: length ≤ **10.00"**, front 2.25" no taller than 2.20". Slots 1-3:
  ≤ **7.00"** (the fan). Height ≤ **2.75"** above the board edge, **3.05"** including
  the tab **V** [TN28].
- Fingers: 25 per side, 0.1" pitch, 1.57 mm board, beveled. TN28 gives a 2.950"
  finger region ending 0.375" from the rear edge; the KiCad `Peripheral_Bus_Edge`
  footprint's tab is 65.8 mm (2.59"). **These disagree -- check against a real card
  before ordering.**
- Templates: [markadev/KiCad-AppleII], [garrettsworkshop/stdpads.pretty]
  (`AppleIIBus_Edge`), [rallepalaveev/a2pico] (complete cards). Check each licence
  before vendoring.

### The memory slot (why it was set aside)

44 pins: multiplexed FRA0-9, /CRAS, /CCAS, CROW0/1, FR/W, D0-D7 (the CPU data bus),
PH2 (bank address valid on D0-D7 at its rise), +5 V at 600 mA max. Full FPI speed,
350 ns cycles. RAM at banks $02-$7F, ROM at $F0-$FD; "for additional memory only".
No /IRQ, /NMI or /DMA in the signal list. RAM2GS puts a CPLD and SDRAM there
**V** [HWR table 9-1, TN68, RAM2GS].

## The boards

```
                 ┌────────────────────────── bus section (one SKiDL subcircuit) ──────────────────────────┐
 Apple II slot   │  A0-15, R/W ──── 74ALVC164245 ─────────────────────────────┐                         │
 50-pin fingers ─┤  D0-7 ────────── SN74LVC8T245 (DIR, /OE from FPGA) ────────┤                         │
                 │  D0-7 ────────── 74LVC541, listen-only (capture) ──────────┤                         │
                 │  PH0 PH1 7M Q3 ─ 74LVC2G17 (+ series-R footprints) ────────┼──► FPGA I/O (3.3 V)     │
                 │  selects, M2SEL, M2B0, RESET-in, INT/DMA IN ─ 5V-tol buffer┤                         │
                 │  /IRQ /NMI /INH /DMA RDY /RESET ◄── 74LVC1G07/2G07 open-drain ◄── FPGA               │
                 │  INT IN → INT OUT, DMA IN → DMA OUT (hardware pass-through)                           │
                 │  /RESET held low until FPGA DONE                                                       │
                 │  +5 V ─ polyfuse ─ bucks ─► 3.3 V (and, on the card, 2.5 V VCCAUX, 1.1 V core)       │
                 │  JTAG 2×5 1.27 mm (rev A only) · UART 1×3 · SAFE jumper · flash WP# jumper             │
                 └─────────────────────────────────────────────────────────────────────────────────────────┘
   carrier (HW-3): bus section + SODIMM-200 socket ── iCESugar-Pro (LFE5U-25F + 32 MB SDRAM + flash)
   card (rev A):   bus section + LFE5U-25F-6BG256C + W9825G6KH-6 + 128 Mbit SPI flash + oscillator, 4 layers
```

### The bus section *(designer's calls, from proven cards)*

- **Data:** SN74LVC8T245, VCCB from the slot's 5 V. Default direction slot → FPGA;
  /OE pulled high so it is off until the FPGA configures. The FPGA turns it toward the
  slot only for a card read (/DEVSEL low, R/W high), from the first 7M↑ after PH0↑
  until before PH0↓. A2FPGA uses the same part for data.
- **Address and R/W:** 74ALVC164245 (A2FPGA's newer boards); it can drive the bus,
  keeping DMA possible.
- **Listen-only data buffer:** a 74LVC541, always on. A driving '245 shows the FPGA
  only its own output; capture needs the real bus, including fights at PH0 edges.
- **Clocks:** 74LVC2G17 with footprints for series resistors (BMOW saw 8 ns Q3 glitches
  after removing one).
- **Open-drain outputs:** 74LVC1G07/2G07 for /IRQ, /NMI, /INH, /DMA, RDY and the reset
  hold. RDY, /DMA and /INH stay released unless the gateware is explicitly armed.
- **Reset hold:** /RESET is held low until the FPGA reports DONE, so the IIgs never
  sees a half-configured card (A2FPGA's CPLD does this).
- **Power:** polyfuse, then buck converters, with bypassing on all four rails. Target
  **≤ 300 mA at +5 V** so other cards keep their share of the 500 mA; measured on the
  carrier. The carrier differs (HW-14): the module makes its own rails.
- **Debug headers:** on rev A, a shrouded 2×5 1.27 mm Cortex-10 JTAG header (VTref =
  3.3 V, nRESET unconnected), which Tigard, FT232H boards, J-Link and CMSIS-DAP probes
  fit; a 1×3 0.1" UART header (TX, RX, GND). The carrier has no JTAG header (HW-13);
  it keeps the UART header (J4) and the SAFE jumper (JP1).

### The integrated card (rev A)

- LFE5U-25F-6BG256C; W9825G6KH-6 (32 MB, 16-bit); 128 Mbit SPI flash; an oscillator.
- Rails: 1.1 V core, 2.5 V VCCAUX, 3.3 V I/O, SDRAM, flash.
- 4 layers. ULX3S escapes a 0.8 mm BGA-381 on 4 layers. JLCPCB's guide asks for
  0.09 mm trace/space and dog-bone vias at 0.8 mm pitch. Via-in-pad is free only on
  6+ layers, so avoid it.
- Outline ≤ 7.0" long × 2.75" above the fingers, so it fits **any** slot, 1-7.
- Rough cost, inferred: FPGA $15 + SDRAM $6 + shifters and power ~$10, plus 4-layer
  boards, gold fingers, and JLC's $3/Extended-part assembly fee and X-ray. Get a real
  quote before promising a number.

## Gateware around the core

`a2nix_card` keeps its port list, plus `mem_ready` (HW-7). New board-level RTL wraps
it, so MAME still verilates only the core:

```
 pins ─► a2bus_phy ──c0n_addr/read/write/wdata/rdata──► a2nix_card ──mem_*──► mem_bridge ─► SDRAM ctrl ─► SDRAM
   │        │ "card drove D"                                                      ▲
   │        ▼                                                                     │ (arbitrated)
   └──► capture (synchronisers, delay line, framer, triggers) ──► rings ───────────┘
                                   └──► readout: JTAG ER1 │ UART
```

- **`a2bus_phy`** turns the pins into the core's `c0n_*` strobes, using the timing
  above. It is the one piece MAME never exercises -- MAME drives the core at transaction
  level -- so it needs its own pin-level tests. Lessons from A2FPGA's bus bugs:
  2-FF synchronisers only, **never clock logic on PH0**, sample through a delay line at
  a fixed offset, and watch synchroniser latency. Theirs, at 54 MHz, made it drive data
  ~100 ns late.
- **`mem_bridge`** sits between the core's 32-bit port and the 16-bit SDRAM: two beats
  per word, D12's 4-way tree-PLRU cache in block RAM, and `mem_ready` low until the
  word is there (HW-7). It shares the SDRAM controller with capture's ring.
- **Clocking:** the core runs at the clock it meets, ~40 MHz (HW-8). Capture samples at
  100 MHz plus `IDDRX1F`, for ≥ 200 MS/s, if `hw-timing` passes there.

### Capture *(designer's calls)*

Three recorders, behind a passive tap:

| Recorder | What | Where | Holds |
|---|---|---|---|
| Cycle | 8 B per PH0 cycle: A16, D8 at a fixed pre-PH0↓ offset, 14 flags incl. /M2SEL and "card drove D", cycle length, a margin byte | SDRAM ring, 2-8 MB, sharing the controller with the heap | every cycle (8.2 MB/s): ~0.5 s per 4 MB; filtered: minutes |
| Edge | change-encoded samples at ≥ 200 MS/s | BRAM ring, 32-64 KB | ~0.4 ms around a trigger |
| Margin | min/max of edge-to-PH0↓ per signal group | registers | forever, ~100 B |

- Triggers follow AtomBusMon: 4-8 masked comparators (address, data, R/W, selects,
  /M2SEL), a sequencer, occurrence counters, pre/post depth.
- Readout is over **JTAG** (ECP5 `JTAGG`, user register ER1 at IR 0x32), using the same
  adapter that programs the card (on rev A the external one on the Cortex-10 header; on
  the carrier the module's iCELink over CMSIS-DAP, **I**, HW-13), and working even
  while the IIgs is hung. The **UART**
  streams filtered card cycles. Exported as VCD / sigrok `.sr` for PulseView.
- Streaming every cycle needs ~8 MB/s, which only a USB-HS FIFO reaches. Triggered
  snapshots plus a filtered stream cover timing diagnosis, so rev A has no onboard USB.
  (The carrier's module has one, for programming and this readout.)

### Programming and recovery *(designer's calls)*

- On the carrier, openFPGALoader through the module's USB (`-c cmsisdap`, HW-13). On
  rev A, openFPGALoader with a Tigard (`-c tigard`, $49) or any FT232H/FT2232H board.
- Flash map: user image at 0x000000, golden image at 0x800000, ECP5 jump table at the
  top (`ecpmulti --golden`). The upper half is block-protected, with WP# on a jumper.
- ECP5 falls back to the golden image on a CRC error or DONE timeout. To also catch a
  valid-but-broken image, the golden boots first and sets a flash flag, then reloads the
  user image, which clears the flag once it is healthy. After a power cycle with the
  flag still set, the card stays in golden. A SAFE jumper forces golden.
- On rev A, JTAG is the last resort and always works. The carrier has no JTAG header: it
  is programmed over the module's USB (HW-13), so there the USB link is the only path.

## How the agent designs a board

```
 hw/
   src/*.py            SKiDL: slot_bus() and the shared helpers
   carrier/, card/     per board: top.py, pins.toml (RTL port → ball/pad → bank → IO
                       standard: that board's ONE pin map), placement.toml, the module map
                       (carrier: icesugar_pro_v1_3.toml), the routed .kicad_pcb (committed)
   tools/              build_board, route, fab, footprints, check_*.py
   tests/              pytest over the above (`hw-tests`)
 rtl/board/            a2bus_phy, capture, mem_bridge, SDRAM controller, per-board tops
```

1. **Source:** SKiDL subcircuits. The FPGA's pin nets come *from* the board's `pins.toml`
   (`hw/<board>/pins.toml`), which also generates the `.lpf`, so schematic and constraints cannot drift.
2. **Netlist** plus a `.kicad_sch` for people to read and review. The schematic is
   schematizer's labels-only mode: the human-readable artefact. The gates read SKiDL's
   netlist, not the schematic.
3. **Board:** created from the netlist, then a placement table applied by script
   (`pcbnew`). The fingers and outline come from a parametric script.
4. **Fanout** *(card only):* a scripted dog-bone generator for the BGA.
5. **Routing:** DSN → Freerouting → SES. Planes as zones.
6. **DRC:** KiCad's (`--severity-error`, `--refill-zones`, rules from the `.kicad_pro`)
   plus our own netlist ↔ board parity, both in `check_drc.py`. Not KiCad's
   `--schematic-parity`, which reads the generated schematic rather than the netlist.
7. **Renders:** per-layer SVGs and top/bottom PNGs. The agent looks at them; so does a
   person.
8. **Fab outputs:** `kikit fab jlcpcb --assembly`, plus the Interactive HTML BOM.

Routing is not reproducible, so the routed `.kicad_pcb` is committed source. The parity
check is what ties it to the SKiDL netlist. Nets must come from pads and the netlist,
never from net labels the agent writes onto tracks: in testing, KiCad silently moved a
hand-written track that touched no pad onto another net, and DRC missed the short.

**Checks** (each with a mutation that must fail it, D14):

| Check | Asserts | Mutation |
|---|---|---|
| `hw-tests` | pytest over `hw/tests` | -- (the tests are the mutations) |
| `hw-intent` | design intent from the netlist: power-up state, daisy chains, rails, module power (HW-15) | each rule has a planted fault |
| `hw-erc` | SKiDL's `ERC()` on the netlist: zero errors | a planted second driver |
| `hw-drc` | KiCad DRC zero errors, zero unconnected; netlist ↔ board parity; power tracks at class width bar short necks (GND exempt) | a deleted track |
| `hw-pinmap` | netlist ↔ the board's `pins.toml` ↔ board-top RTL ports ↔ `.lpf` agree; bank VCCIO consistent | two balls swapped |
| `hw-edge` | finger count, pitch, position, bevel keep-out | one finger moved |
| `hw-bom` | every part has a footprint and an LCSC number | one LCSC number deleted |
| `hw-fab` | the package's contents: layers, drill, one CPL row per assembled ref, renders. Not byte identity: KiCad stamps dates into gerbers | planted faults in a copy of the package |
| `hw-timing` | nextpnr meets the shared card-clock constant (HW-8) and the capture clock on each board top | the constant raised past what routes |
| `hw-rtl` | `a2bus_phy`/capture/`mem_bridge` tests: MAME's `read_c0nx`/`write_c0nx` log expanded into TN68 pin waveforms, with injected skew and edge-of-window cases; readout equals the log | sample offset off by one tick |

**Where a person must look** (the agent will do these poorly, and DRC cannot see intent):
placement and floorplan; BGA escape and the 4-layer power tree; SDRAM and clock routing
(Freerouting does no length matching); any footprint imported from EasyEDA; the edge
connector against a real card; the shifters' direction/enable logic.

## Staging

1. **Research and this design** -- done when the owner approves this document.
2. **Toolchain and carrier** (HW-3). Done when (**done** = by the toolchain-and-carrier
   plan, plan 1; the rest belongs to plans 2 and 3):
   - **done:** `nix develop .#hw` and the `hw-*` checks (tests, intent, erc, pinmap, edge,
     drc, bom, fab) exist, each proven able to fail;
   - **done, review pending:** the carrier is designed through the loop:
     ERC/DRC/parity/pinmap/edge/BOM green, fab outputs and renders produced. The owner's
     review of them is the list under "Owner's review";
   - plan 2: the core has `mem_ready` (HW-7), with the wait-state test and its mutation,
     and MAME and the harness unchanged in behaviour;
   - plan 2: the card clock is one constant (HW-8), and MAME's `CARD_CLOCK_HZ` follows it;
   - plan 3: `rtl/board/` builds a carrier bitstream that meets timing on the iCESugar-Pro
     pinout at that clock, with `a2bus_phy` and a capture skeleton passing `hw-rtl` and
     `hw-timing`. Today `rtl/board/carrier_top.v` is a port-list stub that only
     `hw-pinmap` reads;
   - plan 3: the bring-up procedure (HW-9) is written, with its listen-only bitstream.
3. **Integrated rev A**, designed alongside the carrier's bring-up (HW-9): the same
   gates plus BGA fanout, a 4-layer stackup, and a real JLCPCB quote. Done when it is
   order-ready and reviewed. **Ordered only after the carrier has run in a real IIgs**
   and its findings are folded in.

Ordering the carrier is the owner's call. Without a IIgs, a carrier can be powered,
programmed over the module's USB, and loop-tested with its shifters, but not bus-tested. A *bus
exerciser* -- an RP2350 on a slot extender, generating TN68 cycles -- would test a card
with no IIgs. It is not planned.

## The carrier as built

| | |
|---|---|
| Outline | **80 mm long × 68 mm** above the finger tab, measured from the committed board. Height was raised from 64 mm so routing could complete; the limit is 69.85 mm (2.75", TN28). |
| Stackup | 2 layers, 70 footprints. DRC: 0 errors, 0 unconnected. |
| SODIMM socket | **TE 1473005-4** (LCSC C428482), DDR2 standard type, 5.2 mm module height. Land pattern from TE drawing C-1473005 rev J3, sheet 2; footprint generated by `hw/tools/footprints.py`. |
| Module map | `hw/carrier/icesugar_pro_v1_3.toml`, from the module schematic (sha256 767cd3f1…) at wuxx/icesugar-pro commit 087e48d. 106 SODIMM pins carry PIOs; banks 0, 1, 2, 3, 6, 7 and 8 are all 3.3 V. Each pin's PIO label is checked against Trellis's site name. |
| Power | D1 and VR1 (HW-14). |
| Debug | Module USB, J4, JP1 (HW-13). |
| Routing | Freerouting 2.4.1, which rejects `--router.copperToEdgeClearanceUm`, so edge clearance is DSN keep-out strips. A last-resort A* finisher in `route.py` joins the nets Freerouting leaves, at each net's class width; it refuses to thin power. Power net class ≥ 0.5 mm. Not reproducible, 15-30 min; the routed board is committed source. |
| Silkscreen | R/C reference text is hidden (the iBOM shows designators). About 93 silkscreen warnings are left ungated. |

The module lies over the empty band above J2; nothing tall goes there.

**Fab package:** `nix build .#hw-carrier-fab` (under `/nix/store/…-hw-carrier-fab`):

| File | |
|---|---|
| `gerbers.zip` | JLCPCB gerbers and drill |
| `bom.csv` | 21 lines, LCSC numbers; hand-fitted parts included |
| `cpl.csv` | 66 rows, KiKit's JLC rotation corrections applied; J1, J4, JP1, JP2 excluded (hand-fitted or board copper) |
| `ibom.html` | Interactive HTML BOM |
| `render-top.png`, `render-bottom.png`, `layers/*.svg` | what the owner looks at |
| `schematic.pdf` | the labels-only schematic |

J1, the fingers, is board copper (`Fab = "pcb"`, no LCSC number).

**JLC order options** live on the order form, not in the files: 2 layers, 1.6 mm FR-4,
ENIG, gold fingers yes, 45° bevel, assembly on top side, BOM and CPL from the package.
PCBWay is the alternative for hard gold (HW-4).

## Design loop

Inside `nix develop .#hw`, from the repo root:

| Command | Does |
|---|---|
| `just -f hw/justfile test` | pytest over `hw/tests` |
| `just -f hw/justfile carrier` | netlist, schematic, LPF, ERC report in `build/hw/carrier`; then erc, intent and pinmap gates |
| `just -f hw/justfile netlist` | `carrier`, then the bus section alone and the intent gate on it |
| `just -f hw/justfile board` | fresh unrouted `carrier.kicad_pcb` from the netlist and `placement.toml`; then the edge gate. **Discards a route.** |
| `just -f hw/justfile route [PASSES]` | Freerouting (default 20 passes; minutes), then the DRC and parity gate |
| `just -f hw/justfile fab` | the BOM gate, the package in `build/hw/fab`, then the fab gate |
| `just -f hw/justfile render` | the renders (runs `fab`) |
| `just -f hw/justfile check` | every `check_*.py` locally, in gate order, without re-routing |

Each recipe runs the check's selftest before the check. `nix flake check` runs the same
gates as `hw-*`.

## Owner's review

The agent cannot judge intent. Before ordering, look at the package above for:

1. **Placement** against the slot outline and the neighbouring slots' clearance: the
   socket's 5.2 mm plus the module's components; the module lies over the empty band
   above J2.
2. **The SODIMM footprint** against TE drawing C-1473005 sheet 2, and pin 1 and key
   orientation against a real iCESugar-Pro.
3. **Translator direction and enable nets** in `schematic.pdf`: the 8T245's /OE pulled
   up and DIR pulled down; the 74ALVC164245 the same; the 1T45's DIR.
4. **BOM LCSC numbers** against the intended parts (`bom.csv` carries the MPN).
5. **The finger tab** against a real Apple II card (open question 5): TAB_WIDTH
   65.8 mm (the KiCad footprint) versus TN28's 2.950".
6. **The module's USB connector:** whether it is reachable with the card in a slot.
7. **Residual electrical risks:**
   - the 74ALVC164245 has no Ioff;
   - on USB-only bench power, `+5V` (the slot side, VCCB) is dead while `+3V3` is live;
   - the carrier's buffers drive the module's FPGA pins and could back-power its VCCIO
     if the module's 3V3 lags.
8. **Hidden R/C designators**, and the ~93 silkscreen warnings.
9. **JLC's CPL preview**, to confirm part rotations.

## Open questions only a real IIgs answers

1. Where to sample safely, across machines (A2FPGA's garbage-data bug).
2. What the address and data lines carry during fast cycles.
3. The power supply's real +5 V rating and the voltage under load.
4. The daisy-chain direction.
5. Finger fit: 2.59" versus 2.950".
6. M2B0 on the owner's ROM version.
7. Whether LVC parts fight or glitch at the end of read cycles (BMOW's Yellowstone).

## Left to the builder

Part choices not named here (rev A's regulators, flash, oscillator, polyfuse; the
carrier's are in "The carrier as built"); the stackup; the SDRAM controller (write one, or adapt an open core); the
exact capture record format and the host readout tool (openocd, pyftdi, or a small
script); footprint sources and their licences; Nix packaging details; how a netlist
change updates an already-routed board without losing placement.

## Found while researching, for the software side

Not changed here; these belong to the docs and RTL the software agent owns.

- [REFERENCES.md](REFERENCES.md) says the IIgs throttles to 1.024 MHz for "any
  $C000-$CFFF access". TN68 lists exceptions that run at 2.8 MHz ($C035, $C036, $C037,
  reads of $C02D and $C068, slot ROM set to Internal), and puts an isolated slow cycle
  at ~1.5 µs on average, not ~1 µs. Both bear on the verb-economics numbers.
- [CARD-ISA.md](CARD-ISA.md) says `tb/test_isa.py` asserts the register map, and the
  RTL cites `tb_card.py`. There is no `tb/` directory.
- The core routes at 43.0 MHz on a -6 ECP5, not the 50 MHz MAME models (HW-8), and
  takes 14.7k LUT4 in the default flow against ARCHITECTURE.md's 11.2k `-noabc9` count.
- [HEAP.md](HEAP.md)'s "inserted behind it later without touching the card's logic"
  needs a stall mechanism. HW-7 adds `mem_ready` to the core for this; it arrives as a
  merge from the hardware worktree.

## Sources

- [HWR] Apple IIgs Hardware Reference, APDA draft 1986-11-21:
  https://www2.goldstarsoftware.com/applesite/Documentation/AppleIIgsHardwareReferenceManual.PDF
- [TN28/30/68] Apple IIgs Tech Notes (Developer CD, March 1993):
  https://downloads.reactivemicro.com/Users/Grant_Stockley/Apple%202%20Tech%20Notes%201993%2003.pdf ;
  TN68: https://archive.org/details/IIgs_2523068_Expansion_Card_Design
- [A2FPGA] https://github.com/a2fpga/a2fpga_core ;
  https://www.wiseowl.com/articles/a2fpga-bus-bugs-five-that-lived-in-the-dark/ (2026-03-12)
- Byte Hamr: https://github.com/robertrico/project_byte_hamr
- a2pico: https://github.com/oliverschmidt/a2pico ; boards at https://github.com/rallepalaveev/a2pico
- RAM2GS: https://github.com/garrettsworkshop/RAM2GS
- Big Mess o' Wires: https://www.bigmessowires.com/2021/06/12/clock-glitch/ ;
  https://www.bigmessowires.com/2021/12/22/edge-connectors-enig-plating-and-galvanic-corrosion/ ;
  https://www.bigmessowires.com/2018/02/12/apple-ii-card-electrical-woes/
- [FAQ-PWR] https://www.vincenzoscarpa.it/biblioteca/retrofaq/AppleIIFAQ/Csa2POWER.html
- [apicula#514] https://github.com/YosysHQ/apicula/issues/514 ; [#541] https://github.com/YosysHQ/apicula/issues/541
- AtomBusMon: https://github.com/hoglet67/AtomBusMon/wiki
- ECP5 sysCONFIG (dual boot): Lattice FPGA-TN-02039
- prjtrellis tools (`ecpmulti`, `ecppack`): https://github.com/YosysHQ/prjtrellis/tree/master/libtrellis/tools
- iCESugar-Pro: https://github.com/wuxx/icesugar-pro ; Icepi Zero: https://www.crowdsupply.com/icy-electronics/icepi-zero
- KiCad 10 CLI: https://docs.kicad.org/10.0/en/cli/cli.html ; SWIG removal: https://dev-docs.kicad.org/en/apis-and-binding/pcbnew/
- SKiDL: https://pypi.org/project/skidl/ ; Freerouting: https://github.com/freerouting/freerouting/releases ;
  KiKit: https://github.com/yaqwsx/KiKit
- JLCPCB gold fingers: https://jlcpcb.com/help/article/jlcpcb-gold-fingers ; BGA guidelines:
  https://jlcpcb.com/help/article/243-BGA-Design-Guidelines---PCB-Layout-Recommendations-for-BGA-packages ;
  PCBWay capabilities: https://www.pcbway.com/capabilities.html
- KiCad Apple II templates: https://github.com/markadev/KiCad-AppleII ;
  https://github.com/garrettsworkshop/stdpads.pretty

[apicula#514]: https://github.com/YosysHQ/apicula/issues/514
[#541]: https://github.com/YosysHQ/apicula/issues/541
[markadev/KiCad-AppleII]: https://github.com/markadev/KiCad-AppleII
[garrettsworkshop/stdpads.pretty]: https://github.com/garrettsworkshop/stdpads.pretty
[rallepalaveev/a2pico]: https://github.com/rallepalaveev/a2pico
