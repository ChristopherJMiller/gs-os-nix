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

---

## Amendment (2026-10-31): four layers

**Owner decision (2026-10-31): go 4-layer.** This supersedes "Stay 2-layer". On 2 layers the re-layout did not close in four full rounds (P11–P16, ~9 h of routing). The jam moved each round: first the finger lane, then the U1–U4 channel, then the translator–J2 band, with 12 nets left and 19 DRC errors at best. The unfinished boards already met the electrical goals: worst slot pin 14.4 pF, clock coupled run 0 mm, detour median 1.09. A solid ground plane closes the routing and improves signal integrity. Rev A's ECP5 BGA needs ≥ 4 layers anyway, so the toolchain learns it now.

**Stackup:** JLCPCB JLC04161H-7628, 1.6 mm (the slot takes 1.57 mm). The layers are F.Cu, a 7628 prepreg of ≈ 0.2104 mm (εr 4.4), In1.Cu, a 1.065 mm core, In2.Cu, another 7628 prepreg of ≈ 0.2104 mm, and B.Cu. Copper is 1 oz outside and 0.5 oz inside. Sources: the gsuberland/jlcpcb_autogenerated_stackups data, the biergaizi stackup gist, and JITX's jlcpcb materials (εr 4.4 for 7628). Cite them where the numbers are used. Finish: ENIG, with bevelled gold fingers as before. Through vias only.

**Layers:**
- F.Cu and B.Cu carry the signals.
- In1.Cu is a solid GND plane: KiCad layer type `power`, one GND zone over the whole board.
- In2.Cu is the power plane. A +3V3 zone covers the bus section. +5V and +5V_MOD may take In2 regions, or stay outer-layer Power-class tracks ≥ 0.5 mm, whichever routes cleanly.
- There are no outer-layer GND pours: the plane carries ground. Every GND and plane-net SMD pad reaches its plane by a via at the pad.

**Revised constraints** (the rest of the Global Constraints stand):
- Inner copper stays out of the finger tab: no In1/In2 copper below the tab's top edge. It keeps the board-edge clearance everywhere else, so the bevel never exposes an inner layer.
- The Clock class stays at 0.2 mm / 0.3 mm (P12), and J2_ESCAPE (P16) stays.
- Power-class width (R23/R26) applies to whatever power tracks remain on the outer layers.
- **hw-load gains a via term.** A via through two planes adds capacitance that copper length alone misses. Each via on a J1 net counts its capacitance, computed from the stackup with the standard via formula `C ≈ 1.41·εr·T·D1/(D2−D1)` pF (T, D1 = via pad, D2 = plane antipad, in inches), cited. Also compute the outer-layer microstrip pF/mm from the stackup (IPC-2141). Keep 0.10 pF/mm if the computed figure is ≤ 0.10, and record both numbers in check_load.py's header. The selftest plants a via-heavy net that fails only because of its vias.
- Height: one full attempt at **64 mm** (Tasks 3+4's height, so the board shrinks if 4 layers allow it). If that attempt does not close, 68 mm. Only J1 moves between the two (as in P11).

### Task 5a: The 4-layer board (no routing)

**Files:** Modify `hw/tools/build_board.py` (layer count, stackup, layer types, In1 GND and In2 power zones, the inner-copper keep-out over the tab, no outer GND pours), `hw/carrier/carrier.kicad_pro` and `build_board.project_json` (4-layer rules), `hw/tools/check_edge.py` (inner copper in the tab fails; a selftest plant), `hw/tools/check_load.py` and `hw/carrier/pin_capacitance.toml` (the via term), `hw/tools/check_fab.py` and `hw/tools/fab.py` (In1/In2 gerbers expected; JLC order notes name the stackup), `hw/tools/check_drc.py` if its zone and parity code assumes 2 layers, and the tests. Remove or retire the 2-layer-only GND machinery (outer-pour stitching and bridging, narrow_gnd) where 4 layers make it dead code. Leave no dead flags in the justfile.

- [ ] **Step 1: Failing tests.**
  - The built board has 4 copper layers.
  - In1 is a `power`-type layer holding one GND zone. In2 holds the +3V3 zone.
  - No copper on In1/In2 lies below the tab's top edge.
  - check_edge's selftest plants inner copper in the tab, and it fails for that reason.
  - check_load counts vias, and its selftest plant fails only through via capacitance.
  - check_fab expects In1/In2 gerbers.
- [ ] **Step 2:** Implement. Rebuild the board with `just -f hw/justfile board` at 64 mm, unrouted. Check that the DSN export writes In1/In2 as planes, or, if KiCad does not, that route.py's DSN step adds fanout vias. Record which in the report.
- [ ] **Step 3:** hw-tests, hw-erc, hw-intent, hw-pinmap and hw-edge pass. hw-drc and hw-load may be red only because the board is unrouted (P4). Commit.

### Task 5b: Route the 4-layer carrier; `hw-load` and `nix flake check` green

**Files:** `hw/tools/route.py` (the first pass and the repair passes on F/B with planes; GND and +3V3 by plane vias), `hw/carrier/carrier.kicad_pcb`, `hw/carrier/placement.toml` (height, if 68).

- [ ] **Step 1:** `just -f hw/justfile route` at 64 mm: the first pass (clocks + SLOT_D0–7), then Freerouting, the clock repair and the finisher. If it doesn't close, set 68 mm (move J1 only), `board`, and route again. Stop after three full attempts that don't close, and report the residual.
- [ ] **Step 2: Gates.**
  - hw-drc: 0 errors and 0 unconnected, parity, power widths.
  - hw-edge, including inner copper.
  - hw-load: every J1 net ≤ 15 pF with the via term. List the worst five.
  - hw-erc, hw-intent, hw-pinmap, hw-bom and hw-fab.
  - `just -f hw/justfile check` and `nix flake check`.
  P4 ends here.
- [ ] **Step 3: Metrics,** before (7430efe) and after:
  - detour median, p90 and max;
  - vias;
  - **In1 GND-plane fill under the bus row** (antipad voids), alongside the old B.Cu fill;
  - clock coupled run;
  - worst slot-pin pF;
  - height.
  Commit the routed board.

### Task 6 (amended)

Step 3 also enters HW-17 in HARDWARE.md: 4 layers, the owner's decision, the stackup and the why. It updates the order notes (4-layer, JLC04161H-7628, ENIG, gold fingers) and hw/README.md ("4 layers", the size). The before/after table records the 2-layer attempt's residual as the reason for the change.

---

## Amendment 2 (2026-11-01): deterministic routing

**Owner decisions (2026-11-01):**
- **Build deterministic routing** for the bus section, instead of widening the board, giving In2 to signals, or finishing by hand.
- **Switch to Freerouting 2.5.0**, pinned in the flake. Its output is identical on every run, so routes become reproducible.

**Why.** Seven auto-routing rounds left 8–15 nets open each time, wherever the router happened to jam. They covered 2 and 4 layers, 64–69.5 mm, and Freerouting 2.4.1 and 2.5.0. The module covers the top ~45 % of the board, so every part sits in a ~32 × 80 mm strip. A net-by-net router can't plan that strip globally.

Research (2026-10-31; reports in the SDD directory as `research-*.md`) points to doing what practitioners do by hand:
- plan the buses as ordered, uncrossed rivers;
- break out the fine-pitch connector deterministically;
- use the empty area under the module as the lateral "highway";
- leave only the sparse remainder to the autorouter;
- check routability in seconds before spending hours on a route.

On the best partial board, the area under the module carries 4 traces.

**Facts the tasks rely on:**
- J2 has two pad rows, 4.1 mm apart, with pads 0.35 × 2.0 mm, leaving a ~2.1 mm gap between pad ends. Rear-row pads can be reached from above (under the module) on F.Cu.
- The module's FPGA I/O is only on SODIMM pins 41–156.
- J1's fingers are on both sides, pin n on F and pin 51−n on B.
- A 245's or 541's channels are interchangeable bit by bit (Task 1 and Tasks 3+4 already permuted them), so channel order is free on each translator.
- Copper may run anywhere under the module: only courtyards, not tracks, are excluded there.

### Task 5d: Freerouting 2.5.0 in the flake

**Files:** `nix/pkgs/freerouting.nix` (new), `nix/hw.nix`, `hw/tools/route.py` and `hw/tests/test_route.py`.

- Package the release jar with `fetchurl`, sha256 `f6f51bb02245e8e717f9359bd260cc9c5c0b1bc0acc8b7cb2cd5b8ffeb5de3c7`, wrapped to run on `jdk25_headless`. The draft is at scratchpad `fr25/freerouting.nix`.
- route.py's flags become `--router.autorouter.max_passes=N` and `--router.optimizer.max_threads=N`, since `-mt` alone does not cap threads. Freerouting runs with an isolated HOME or config directory, so it never writes `~/.config/freerouting`.
- Add a test that two routes of a tiny board give byte-identical SES. Run it in hw-tests only if it takes under ~30 s; otherwise put it behind a marker the justfile runs.

### Task 5e: `hw-congestion`, a routability gate before routing

**Files:** `hw/tools/check_congestion.py` (new), its tests, `hw/justfile`, `nix/hw.nix`.

- A coarse global router on a ~0.5 mm grid, in the PathFinder style with negotiated congestion. Each cell's capacity per signal layer comes from track pitch, minus pads, vias (including the plane fanout), courtyard-blocked F.Cu and keep-outs. B.Cu under top-side parts is open.
- It routes every signal net's minimum spanning tree and reports the total overflow, the worst cells and the nets through them. It also writes a heat-map PNG for review.
- Pass/fail: overflow 0 after N negotiation rounds.
- Calibration:
  - plan 1's routed board (7430efe) must pass, since it routed;
  - planted faults (a narrowed band, a blocked channel) must fail, each for its own reason;
  - the three jammed 4-layer boards (64, 68 and 69.5 mm, with the fanout) are recorded, not asserted, because a better router might close them.
- `--selftest` per D14. Wire it into `gate-board`, `check` and `checks`, before routing in `route`.

### Task 5f: ordered (monotone) bus assignment

**Files:** `hw/tools/assign_pins.py` (or a new `plan_buses.py` that it calls), `hw/carrier/pins.toml`, `hw/src/slot_bus.py` if channel order changes, and the tests.

For each translator, choose the channel permutation and the J2 pads so that each bus is a river:
- J1 finger order matches the A-side pin order (per finger side);
- the A side maps to the B side through its channel;
- the B-side pin order matches the J2 pad order (per J2 row);
- no two nets of a bus cross.

The method is dynamic programming over ordered matchings (the research report's monotone DP), minimising lateral travel. Which J2 row each net uses is part of the choice: nets that must travel far laterally take rear-row pads, reached through the highway.

Keep every existing check: the module map, hw-pinmap, the LPF and carrier_top.v, hw-intent's channel pairing, and `crossings()` as a reported metric. Show RED first: a test where a bus crosses itself under today's assignment and is planar after.

### Task 5g: the bus router

**Files:** `hw/tools/bus_route.py` (new), its tests, and `route.py` (it calls bus_route before Freerouting).

Lay the bus section's nets deterministically and lock them, with no randomness:
- **Slot side:** J1 fingers → translator A-side pins, as short rivers. These lengths feed hw-load.
- **FPGA side:** translator B-side pins → J2.
  - Each net gets a short stub and a via.
  - It runs vertically to J2 on one layer.
  - Lateral shifts happen in the highway under the module, on the layer chosen there, with tracks assigned by the left-edge algorithm.
  - Rear-row pads are entered from above on F.Cu, and front-row pads from below.
  - Fixed via positions break out J2 in the inter-row gap or under the module.
- Obstacles it cannot pass are reported, never forced. Nets it can't lay are left to Freerouting and listed.
- Respect the Clock class (P12), J2_ESCAPE (P16) and the plane fanout (P19), which bus_route may re-place if a fanout via sits in a river's path. Never put a via in the tab.
- Tests on stub boards: a river of N nets stays planar, is DRC-clean, is locked, and is byte-identical across runs.

### Task 5h: route the carrier

`just -f hw/justfile route` runs, in order: congestion gate → fanout → bus_route → first pass (clocks, if bus_route doesn't own them) → Freerouting 2.5.0 → repair and finisher. The placement is the committed 69.5 mm layout (P22), unless hw-congestion names a better one and a ruling adopts it.

Acceptance is Task 5b's Step 2:
- hw-drc 0/0;
- hw-load ≤ 15 pF on every J1 net;
- every gate passes;
- `just check` and `nix flake check` pass.
Then the metrics. Because routes are now deterministic, `route` reproduces the committed board, and that is checked.

### Task 6 (amended again)

HARDWARE.md also records the routing strategy (HW-18): deterministic buses plus an autorouter for the remainder, and why. It cites the research where it was verified.

---

## Appendix: the controller's rulings P1–P29

The code and HARDWARE.md cite these as P*n*. They were made during execution, within the owner's decisions, and each is recorded with its cost if wrong. Later rulings supersede earlier ones where they say so:
- P11 supersedes P10;
- P22 supersedes P11's "J1 only";
- P15's routed GND and P13's GND bridging belong to the 2-layer board, which HW-17 retired;
- P20's Freerouting is now 2.5.0 (HW-18).

- **P1:** Tasks 3 and 4 run as one dispatch, in this order: placement (T4 Steps 1–2), pin assignment (T3), then T4 Steps 3–4. They are reviewed as one unit — the plan itself orders them so — cost if wrong: one larger review.
- **P2:** T4 picks the smallest height at which placement fits with courtyards clear and ≥ 3 mm routing channels between rows. T5 may raise it (in 2 mm steps, ≤ 68) if routing fails, and records the final height — routability is only known in T5 — cost if wrong: a re-route.
- **P3:** T2 adds a standalone `load` recipe only. `gate-board`, `check` and `checks.hw-load` get it in T5, when the board passes — keeps `just check` green between tasks — cost: none.
- **P4:** from T1 until T5 the board-parity gates (hw-drc, hw-load, hw-fab) may be red, because T1 changes the netlist and T4 rebuilds the board. Netlist-level gates (hw-tests, hw-erc, hw-intent, hw-pinmap) stay green at every commit; `nix flake check` is green again at the end of T5 — the board is being replaced anyway — cost if wrong: none (the branch merges only at the end).
- **P5:** hw-load follows each J1 net through 0 Ω parts (value "0", "0R", "0Ω"), counting the downstream copper and pins as the same load. Other resistors stop the walk. Jumpers and headers count 1 pF — a 0 Ω link is a wire — cost if wrong: a clock load under-counted.
- **P6:** the data translator is SN74LVCR2245ADBQR (LCSC C2675819, SSOP-20 0.635 mm, 254 in stock), with the ADBR (C2865353) as fallback — the TSSOP is out of stock and an open-hardware board must be orderable; Task 4 re-places anyway — cost if wrong: a footprint swap.
- **P7:** Tasks 3+4 may add decoupling capacitors to the schematic (slot_bus.py): one 100 nF per IC supply pin, plus 100 nF on each of the four slot rails at the fingers, with tests and counts updated and netlist gates green — the plan's Task 4 asks for that decoupling, which needs netlist parts — cost if wrong: a few extra 0603s.
- **P8:** every component stays on F (top); slot-side traces from B.Cu fingers rise on B.Cu and via up — single-sided JLC assembly is cheaper and simpler — cost if wrong: longer data stubs (hw-load will tell).
- **P9:** /IOSEL's 37.7 mm run to U5 is accepted (~3.8 pF copper + 4 pF ≈ 8 pF < 15); Task 5's hw-load is the check; a dedicated 1G17 is the fallback — cost if wrong: one extra part later.
- **P10:** height 64 mm (not 62) is deliberate, to keep J2's art band — height doesn't change the price (audit) — cost: none.
- **P11:** height 68 mm (P2's cap, plan 1's height) — the owner's "shrink if the re-layout allows" — it doesn't at 64/66; only the fingers move — cost: none (height doesn't change price).
- **P12:** Clock clearance 0.3 mm, not 0.4 — on a 1.6 mm 2-layer board coupling is set by the ~1.5 mm height over the B.Cu plane, so 0.4 vs 0.3 mm changes crosstalk little; coupled length is the lever (route_metrics reports it) and the slot clocks pass through Schmitt 2G17s — cost if wrong: marginal crosstalk on the clocks.
- **P13:** the route closes with deterministic post-passes after Freerouting, before stitching: (a) clock-clearance repair — rip non-Clock copper that violates the Clock clearance and re-lay it with the finisher, iterated; (b) GND-fragment bridging — a via per unconnected F.Cu GND fragment over B.Cu GND, refill, iterated. Stop and report (no other part moves, no >68 mm, keep the first pass) if 3 full attempts at 68 mm don't close — Freerouting alone can't be made to honour a class halo — cost if wrong: more tooling.
- **P14:** passives whose courtyards sit in the U1–U4 channel (R14, the DIR pull, at least) move out of it; decoupling caps keep ≤2 mm to their pin (the existing test); no IC moves. Fallback in the same round if the channel still blocks: row 2 (U4, U5 and up through J2) moves up ≤2 mm into J2's art band, only if the art still clears J2's courtyard — cost: a small placement change.
- **P15:** GND goes back into the DSN as a routed net in the Power class (plan 1's way); pours add to it; bridging vias may sit inside passive courtyards (tented, ≥0.2 mm from pads) as a backup; stitching keeps the board-edge clearance — plan 1's lesson — cost: GND tracks take some room.
- **P16:** the Clock-clearance exemption covers J2's pad escape: a named rule area (J2's courtyard grown 2 mm) used by the .kicad_dru, not the courtyard itself — 0.5 mm-pitch fan-out can't hold 0.3 mm until it leaves the socket — cost: clocks may run 0.15 mm from a neighbour for ≤2 mm at the socket.
- **P17:** the via term stays once per via (Johnson's form, T = the whole board), documented as such in check_load.py's header with the margin argument: the copper term's 0.10 vs the computed 0.073 pF/mm leaves ~27 % headroom — cost if wrong: ~0.6 pF/via under-count.
- **P18:** inner layers are plane-only: any track on In1/In2 fails check_edge (a plant) — the stackup has no inner signal routing — cost: none.
- **P19:** plane connections by a scripted fanout before routing — one locked, tented via within 1 mm of every GND/+3V3 SMD pad, none in the tab; edge-finger GND by a track out of the tab to a via; THT pads join the planes with thermals — deterministic connectivity, short decoupling loops — cost: ~100 vias.
- **P20:** prepare_dsn keeps the (plane ...) entries; if Freerouting 2.4.1 mishandles them, strip again and rely on the fanout — cost: none.
- **P21:** vias stay 0.6/0.3 mm (JLC no-surcharge; 5a's via term assumes it) — cost: slightly larger vias.
- **P22:** widen the band: J2 and the top-edge headers (J4, JP1, JP2) move up 2 mm; height 69.5 mm (≤ 69.85) with every other part moving down 1.5 mm with J1 (supersedes P11's J1-only rule; rows keep their relation to the fingers). Band above U4: 3.2 → 6.7 mm. Art (y 9.8–27.9) stays inside the module outline (5.5–37.6) — cost: 1.5 mm taller, no price change.
- **P23:** assign_pins is re-run after the move (its own documented procedure) and adopted if crossings don't rise; SLOT_RESET_N and SLOT_IOSEL_N join the first pass (laid short and locked) — the two J1 nets nearest 15 pF — cost: none.
- **P24:** route.py must not rewrite carrier.kicad_pro (test-pinned) — cost: none.
- **P25:** Tasks 5f and 5g run as one dispatch, reviewed as one unit — the bus router's topology defines what the assignment must optimise (as P1 did for Tasks 3+4) — cost if wrong: one larger review.
- **P26:** Task 5e (hw-congestion) runs in parallel in an isolated worktree on a branch from 6448116, cherry-picked here when reviewed — it owns new files plus small justfile/hw.nix hunks — cost: a merge of two small hunks.
- **P27:** tented vias under IC bodies (SLOT_D under U1/U4) are accepted — common practice, nothing on the bottom side, mask-tented — cost if wrong: none at assembly (single-sided).
- **P28:** a plane SMD pad may share a via with a same-net pad ≤ 1 mm away, joined by a short track (pin → decoupling cap → via, the shortest loop); no via may overlap any SMD pad's copper — a new hw-drc rule with a selftest plant; the route never aborts for lack of via room when a share exists — cost: none.
- **P29:** bus_route lays +5V and +5V_MOD as planned Power-class paths (≥ 0.5 mm, locked): +5V_MOD through the highway under the module, +5V to U2's VCCA pins (7, 18) under U2's body on B.Cu; report what it can't lay — the probe shows they are the only nets the remainder can't close — cost: two more planned paths.
