<!-- SPDX-License-Identifier: CERN-OHL-S-2.0 -->

# hw — the physical card

![The carrier, isometric: every package, the socket body and the silkscreen art](img/carrier-iso.png)

The open-hardware side of gs-os-nix: Apple II slot cards designed as code. SKiDL
writes the netlist, scripts place and route the board in KiCad 10, and mutation-proven
gates check it. The design, its decisions and the owner's review checklist are in
[docs/HARDWARE.md](../docs/HARDWARE.md).

## The bus carrier

The first board is a development carrier, not the release card (HW-3). It holds the
**bus section** that the integrated card will reuse unchanged: the Apple II edge
fingers, level translators, input and listen-only buffers, open-drain outputs and the
reset hold. The data translator is an **SN74LVCR2245A**, whose outputs have 26 Ω series
resistors built in, to tame the overshoot on the Apple bus (HW-16). A DDR2 SODIMM-200
socket takes an **iCESugar-Pro** module (LFE5U-25F, 32 MB SDRAM).

| Top | Bottom |
|---|---|
| ![The carrier, top, 3D](img/carrier-top.png) | ![The carrier, bottom, 3D](img/carrier-bottom.png) |

The silkscreen carries the name, the open-source-hardware gear and, in the band above J2 that
the module covers once it is plugged in, an Apple II and the NixOS snowflake in ASCII. A
script places the reference designators clear of pads and other silkscreen; the 8 it finds no
room for are named on the fab layer and in the interactive BOM.

- 4 layers: signals on the outside, a ground plane and a 3.3 V plane inside (HW-17).
  80 mm × 69.5 mm above the fingers. It fits any slot, 1–7.
- Every slot pin loads the bus with at most 15 pF, Apple's limit (`hw-load`; the worst is
  14.3 pF).
- The module lies flat over the empty band above the socket. Its own USB programs it
  and carries capture readout (HW-13). The carrier adds a UART header (J4) and two
  jumpers: SAFE (JP1) and the reset-hold bypass (JP2).
- Power: slot +5 V → polyfuse F1 → Schottky D1 → module; an AP2112K makes the
  translators' 3.3 V from the module side (HW-14).
- DRC is clean (0 errors, 0 unconnected, no warnings), and the board matches the netlist pin
  for pin.

**How it's routed** (HW-18). The buses are planned first: each FPGA-side bus takes J2
contacts in the order of its translator's pins, so its nets never cross. Our own router,
`bus_route.py`, then lays those buses, the clocks and most of the 5 V rails, deterministically, and
locks them. Freerouting 2.5.0 routes the remaining nets, an A* finisher joins whatever it
leaves, and every step gives the same copper on every run of the flake's pinned inputs (KiCad 10.0.6,
the Freerouting 2.5.0 jar on JDK 25, 6 router threads, the `route` recipe's 20 passes; checked on
one machine): `just -f hw/justfile repro` routes the board again and checks that it does. Bumping
nixpkgs or the jar means running `route` and `repro` again.

**Status:** designed, routed and gated. It is not yet fabricated, and not yet tested in
a IIgs. Rev A, the integrated ECP5 card, is ordered only after this carrier has run in
a real machine (HW-9).

## Layout

| Path | What |
|---|---|
| `src/` | SKiDL subcircuits; `slot_bus.py` is the bus section |
| `carrier/` | The carrier: `top.py`, `pins.toml` (the FPGA pin map), `placement.toml`, the module's SODIMM map, `artwork.toml` (the silkscreen art), the routed `carrier.kicad_pcb` |
| `lib/` | Generated: `gsnix.pretty/` (the slot fingers and the SODIMM socket) and `gsnix.3dshapes/` (an approximate socket body, for renders only) |
| `tools/` | Board builder, bus planner and router, the route, the silkscreen, fab export, and the `check_*.py` gates |
| `tests/` | The tools' tests (`hw-tests`) |
| `img/` | The pictures above, refreshed with `just -f hw/justfile readme-images` |

## Using it

```sh
nix develop .#hw
just -f hw/justfile check          # every gate, each selftest first
just -f hw/justfile fab            # gerbers, BOM, CPL, renders in build/hw/fab
nix build .#hw-carrier-fab         # the same order package, from Nix
just -f hw/justfile artwork        # (re)apply the silkscreen art and place the designators
just -f hw/justfile route          # route the board again (about 15 min); then `artwork`
just -f hw/justfile repro          # check that `route` gives the committed copper (as long)

nix develop .#hw-render            # optional: the hw shell plus KiCad's 3D models (a big download)
just -f hw/justfile readme-images  # the 3D renders in hw/img
```

`nix flake check` runs the same gates as `hw-*` checks. It and `nix develop .#hw` never
fetch the 3D model library; only `.#hw-render` does.

## Credits

- The Nix snowflake in the silkscreen art is drawn after the NixOS logo (CC-BY 4.0,
  [NixOS/branding](https://github.com/NixOS/branding)).
- The open-source-hardware gear is the silkscreen symbol from KiCad's footprint library
  (`Symbol.pretty`, CC-BY-SA 4.0 with the library's design exception).
- The 3D renders use KiCad's 3D model library, and our own approximate socket body.
- [Freerouting](https://github.com/freerouting/freerouting) (GPL-3.0) routes the nets our
  bus router leaves; the flake pins its 2.5.0 release.

## Licence

Board files are **CERN-OHL-S-2.0** ([LICENSE](LICENSE)). The tools, the tests and the
RTL are GPL-3.0-or-later, like the rest of the repo.
