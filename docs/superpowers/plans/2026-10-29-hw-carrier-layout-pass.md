# Carrier layout pass -- Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Re-place and re-route the bus carrier so that every slot pin meets Apple's 15 pF load limit. The card's data outputs get series termination (74LVCR2245), the ground pour stays whole under the bus, and the clocks are routed clean. A new gate keeps the load limit true.

**Architecture:** The schematic changes only in the data translator (SN74LVC8T245 → 74LVCR2245A). Everything else is placement, pin assignment and routing, driven by the existing tools: `build_board.py`, `route.py`, `artwork.py`, plus one new gate, `check_load.py`. New pin assignments are scripted and scored by ratsnest crossings. Routing goes in two passes: clocks and data by script first, then Freerouting.

**Tech Stack:** As plan 1 (KiCad 10.0.6 SWIG, SKiDL 3.0.0, Freerouting 2.4.1, KiKit), in `nix develop .#hw`; renders in `nix develop .#hw-render`.

**Spec:** `docs/HARDWARE.md` (HW-3, HW-13..15), plus the 2026-10-29 layout audit and design research. Their findings are in this plan's "Why" section, because the reports live in the session scratchpad.

## Why (the audit and the research, 2026-10-29)

Measured on the routed board:
- Median detour is 0.99× the ideal, but SLOT_D5 is 2.45× and slot_dl* / slot_a14/15 are ~2×.
- **SLOT_D0–D5 run 52–129 mm** from the fingers to two buffers each. That puts them at an estimated **18–23 pF per pin**, over Apple's **≤ 15 pF and ≤ 2 LS loads** (HWR 5-13). D6, D7 and /RESET are at ~15 pF. The estimate uses 0.1 pF/mm and 5 pF per input.
- The clocks run 15–24 mm alongside other signals at ≤ 0.3 mm.
- B.Cu ground under the translator row is only ~33% filled, chopped up by traces.
- Ratsnest crossings: **1093**. A global FPGA pin re-assignment is estimated to bring that to **~592**; re-sorting within each bus alone gives only 1069.
- Skew is irrelevant: the worst bus spread is 0.51 ns against 30–140 ns margins. **No length matching.**

Research (Big Mess o' Wires' Yellowstone, RAM2GS II):
- Strong LVC edges into the Apple bus overshoot badly. Yellowstone 2.x moved to the **74LVCR2245** (26 Ω outputs built in), put the transceivers at the fingers, kept data away from A0–A6, and improved its ground fills. It stayed on 2 layers.

Price: under 100 × 100 mm, area does not change JLCPCB's price (inferred), so height is shrunk only if placement frees it.

## Owner decisions (2026-10-29)

- Full pass.
- The data translator becomes **74LVCR2245**.
- Stay **2-layer**.
- **Shrink the height if the re-layout allows** (today 68 mm above the tab; the limit is 69.85).

## Global Constraints

- Branch `hw/toolchain-carrier` in worktree `.claude/worktrees/hw-toolchain-carrier`. Same file-scope rule as plan 1 (outside `hw/`, `nix/hw.nix`, `nix/pkgs/` and `rtl/board/`, only `flake.nix` minimally and `docs/HARDWARE.md`).
- Every plan-1 gate stays green. Every new gate has a `--selftest` whose planted fault fails for its own reason (D14).
- Nets come only from pads and the netlist. kicad-cli and KiKit run only on temp copies (R21).
- Slot-pin load: **≤ 15.0 pF** per J1 net, estimated as routed copper length × **0.10 pF/mm** + Σ input/IO capacitance of every IC pin on the net (datasheet Ci/Cio values, cited in a table). It must hold for every J1 signal net.
- Power class ≥ 0.5 mm (R23, with R26's necks). Signals 0.15/0.15. A new **Clock** net class (`SLOT_PH0 SLOT_PH1 SLOT_Q3 SLOT_7M slot_ph0 slot_ph1 slot_q3 slot_7m`): 0.2 mm track, **0.4 mm clearance**.
- Outline ≤ 177.8 × 69.85 mm above the tab; the finger geometry is unchanged. Height is whatever placement allows, ≤ 68 mm.
- The silkscreen art is re-applied after routing (`just -f hw/justfile artwork`). Its gates (test_artwork) stay green.

## Review Focus

1. **The LVCR2245's DIR/OE polarity and its single 3.3 V supply.** A wrong DIR default drives the IIgs bus at power-up. → Task 1 polarity test and hw-intent rules.
2. **The load gate's capacitance table.** A wrong Ci makes the gate pass a real overload. → Task 2 cites each value, with a test pinning the table to the netlist's part values.
3. **Pin re-assignment breaking the module map.** → Task 3 runs hw-pinmap and test_icesugar_map.
4. **Clock routing by script leaving stubs or bypassing parity.** → Task 5: hw-drc parity, zero dangling ends.
5. **Ground continuity claims.** → Task 6 measures B.Cu fill under the bus row before and after, and records both.

---

### Task 1: The data translator becomes 74LVCR2245A

**Files:** Modify `hw/src/slot_bus.py`, `hw/tools/check_intent.py`, `hw/tests/test_slot_bus.py`, `hw/tests/test_check_intent.py`, `docs/HARDWARE.md` (a new HW-16 entry: the swap, with BMOW's Yellowstone as evidence; the bus-section table).

**Interfaces:**
- **Produces:** the data translator U1 = `74LVCR2245A`, TSSOP-20, VCC = `+3V3`. A side = FPGA (`slot_d0..7`); B side = slot (`SLOT_D0..7`).
- **Defaults:** `DIR` on `slot_d_dir` pulled down (B→A, slot to FPGA); `/OE` on `slot_d_oe_n` pulled up.
- **Listen buffer:** the listen-only '541 still taps `SLOT_D*` directly.
- **Rules that change:** the dual-supply rule for the data translator, since it no longer has a +5 V pin. **Rules that don't:** "no 3.3 V-only part on +5V" now covers `74LVCR2245A`.

- [ ] **Step 1:** Verify the part: symbol pin-compatible with KiCad's `74xx:74LVC245`, or confirm a dedicated symbol; the datasheet's DIR/OE function table; the 26 Ω on which ports; 5.5 V-tolerant I/O; Ioff; an LCSC number in stock, with MPN. Record each with its citation in the slot_bus.py comment.
- [ ] **Step 2: Failing tests.**
  - `test_lvcr2245_polarity`: pins DIR, /OE, A1..8 = `slot_d0..7`, B1..8 = `SLOT_D0..7`, VCC = `+3V3`.
  - The intent mutations: `lvcr_on_5v` and `dir_pull_to_3v3` (the latter on the new part) must trip.
- [ ] **Step 3:** Swap the part. Update the intent rules. Run `just -f hw/justfile netlist`; the hw-intent and hw-erc selftests and real runs pass.
- [ ] **Step 4:** Commit "hw: the data translator is a 74LVCR2245A (26 Ω outputs), after BMOW's Yellowstone".

### Task 2: `hw-load` -- every slot pin ≤ 15 pF

**Files:** Create `hw/tools/check_load.py`, `hw/carrier/pin_capacitance.toml` (part value → per-pin-type input/IO capacitance in pF, each with a datasheet URL and page), `hw/tests/test_check_load.py`. Modify `nix/hw.nix` (`checks.hw-load`, after hw-drc's netlist), `hw/justfile` (the `gate-board` and `check` recipes).

**Interfaces:**
- **Library call:** `problems(board: pcbnew.BOARD, nl: Netlist, caps: dict, limit_pf: float = 15.0, pf_per_mm: float = 0.10) -> list[str]`. For each net with a J1 pad except the power and GND nets, the load is routed length (all tracks on the net) × pf_per_mm plus Σ capacitance of every non-J1 IC pin on the net. Passives and connectors count 0, except jumpers and headers, which count 1 pF.
- **CLI:** `check_load.py [--selftest] --netlist N BOARD`. It prints a per-net table, then `HW LOAD OK`.
- **Selftest mutations,** each matched to its own message: one SLOT_D track lengthened by 60 mm; a third buffer's input added to a SLOT_D net; one Ci in the table raised to 20 pF.

- [ ] **Step 1: Failing tests.** `test_check_load.py` covers:
  - the table covering every IC value in the carrier netlist;
  - the arithmetic on a synthetic board;
  - each mutation tripping.
- [ ] **Step 2:** Implement and wire it up. Run it on the **current** board: it must FAIL on SLOT_D0..D5. That is the audit's finding, now a gate. Record the per-net table.
- [ ] **Step 3:** Commit, with the gate wired in and the current failure documented. **The `hw-load` check is excluded from `checks` until Task 5** (so `nix flake check` stays green between commits): add it to `checks` in Task 5.

### Task 3: FPGA pin re-assignment, scored by ratsnest crossings

**Files:** Create `hw/tools/assign_pins.py`, `hw/tests/test_assign_pins.py`. Modify `hw/carrier/pins.toml`.

**Interfaces:**
- `crossings(board, nl) -> int`: count crossing pairs of straight pad-to-pad ratsnest segments, using each net's minimum spanning tree.
- `propose(board, nl, module, pins) -> PinMap`: re-assign the port bits' balls among the module's free 3.3 V PIO SODIMM pins to minimise crossings; a greedy or simulated-annealing search is fine. It respects `on_module` and keeps each bus's bit order monotonic along the socket where possible.
- **CLI:** `assign_pins.py --report` (current and proposed counts) and `--write`.
- **Constraint:** the placement used for scoring is Task 4's new placement. So run Task 4's placement first, then this, then route.

  **Ordering ruling for the executor:** do Task 4 Steps 1–2 (placement), then Task 3, then the rest of Task 4.

- [ ] **Step 1: Failing tests.** `crossings` on a synthetic 2-net crossing board gives 1 and on a parallel one gives 0. `propose` never assigns a ball twice, nor a non-3.3 V or non-PIO ball.
- [ ] **Step 2:** Implement. Report: current 1093 (or whatever the new placement gives) versus proposed. Write pins.toml.
- [ ] **Step 3:** hw-pinmap, test_icesugar_map and hw-erc pass, and `carrier_top.v` is unchanged (only balls change).
- [ ] **Step 4:** Commit with the before/after counts.

### Task 4: Re-placement for short slot nets

**Files:** Modify `hw/carrier/placement.toml`. Possibly modify `hw/tools/build_board.py`, if the board height must come from placement (it already reads `[board] height_mm`).

- [ ] **Step 1:** Floorplan against the finger order. Pins 1–25 sit on F.Cu with pin 1 at the front; pin n is opposite 51 − n.
  - D7..D0 = pins 42..49 (B.Cu, front half): the 74LVCR2245 and the listen '541 go directly above those fingers.
  - A0..A15 = pins 2..17: the 74ALVC164245 above them.
  - The clocks (36–38, 40) and the selects (1, 20, 39, 41): the 2G17s and the control '541 directly above their fingers.
  - The open-drain '1G07s near 21, 22, 29–32.
  - The polyfuse, D1 and VR1 near pin 25/26 (+5V/GND).
  - Decoupling within 2 mm of each supply pin; both VCC pins of every translator get their own 100 nF.
  - All four slot rails get a 100 nF at the fingers.
  - Then J2 above, with the art band and the headers.
- [ ] **Step 2:** Run `build(update=False)` into a fresh board (the old routing is discarded). Check courtyards and render.
- [ ] **(Task 3 runs here.)**
- [ ] **Step 3:** Try reducing `[board] height_mm` from 68 in 2 mm steps while placement still fits with routing room. Record the smallest that routes in Task 5. **Owner's rule:** shrink only if the layout allows it.
- [ ] **Step 4:** Commit placement.toml and the unrouted board.

### Task 5: Routing in two passes, and `hw-load` green

**Files:** Modify `hw/tools/route.py` (a `--first NETS` pass), `hw/carrier/carrier.kicad_pro` (the Clock net class), `hw/carrier/carrier.kicad_pcb`, `nix/hw.nix` (add `hw-load` to `checks`).

**Interfaces:** `route(board, passes, timeout_s, first: list[str] | None)`. The `first` nets (clocks, then SLOT_D*/slot_d*) are laid by the existing A* finisher, at their net-class width and clearance, as short as it can make them. They are locked before the DSN goes to Freerouting, which routes the rest.

- [ ] **Step 1: Failing test.** `test_route.py::test_first_nets_are_routed_first_and_locked`: on a tiny board, the first nets are laid and locked, and Freerouting's pass doesn't move them. Use a stub router for that pass.
- [ ] **Step 2:** Route the carrier, then refill zones. Add GND stitching vias by script in the bus region: a grid ≤ 5 mm, skipping keep-outs.
- [ ] **Step 3: Gates.** hw-drc (0/0, parity, power widths), hw-edge, **hw-load (every J1 net ≤ 15 pF)**, hw-erc, hw-intent and hw-pinmap all pass.
- [ ] **Step 4:** Commit with the per-net load table and the detour stats.

### Task 6: Art, renders, fab, docs, and the before/after

**Files:** Modify `hw/carrier/carrier.kicad_pcb` (art re-applied), `hw/img/*`, `docs/HARDWARE.md`, `hw/README.md`.

- [ ] **Step 0:** Redraw the ASCII snowflake in `hw/carrier/artwork.toml` so it reads as the NixOS six-lambda snowflake. The 2026-10-29 version reads as a generic burst. Judge it from the top render; keep the 1.6 mm row pitch (tighter added silk_overlap warnings).
- [ ] **Step 1:** `just -f hw/justfile artwork`. test_artwork passes. Re-render the README images in `nix develop .#hw-render` (`just -f hw/justfile readme-images`), and look at them.
- [ ] **Step 2: Re-measure.** Re-run the audit measurements and put the before/after table in HARDWARE.md:
  - detour median, p90 and max;
  - via count;
  - ratsnest crossings;
  - B.Cu GND fill % under the bus row;
  - the worst slot-pin pF;
  - the clock parallel run;
  - board height.
- [ ] **Step 3: Docs.** Update HARDWARE.md (HW-16 entered in Task 1; the gates table gets `hw-load`; board size; the owner's review list) and hw/README.md (size, the LVCR2245).
- [ ] **Step 4:** `nix flake check` passes, and `nix build .#hw-carrier-fab` builds. Commit.
