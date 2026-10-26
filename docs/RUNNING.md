# Running 65816 code against the card

Three routes, for three different jobs. All need `nix develop`.

| Route | Command | ROMs? | Job |
|---|---|---|---|
| **0. Minimal harness** | `just m2` | **no** | The gating M2 check. Real binary, real RTL, no OS |
| 1. MAME debugger | `just m2-mame` | yes | Higher-fidelity confirmation |
| 2. ProDOS disk | `just disk` | yes | Interactive, and the route for L4+ |

## Route 0 — minimal 65816 harness (the gating check)

```sh
just m2
```

`sim/a2min.cpp` is 1 MB of RAM, a slot decoder, and a 65816 interpreter (emulation
and native mode) wired directly to the Verilated card. It executes the **same binary** against the **same
RTL** as the other routes, with no ROM and no copyrighted material — so it runs in
`nix flake check` on any machine.

It exists because `apple2gs` will not start without all four of its ROMs, one of which
is only the character-generator font (`region="gfx1"`) that a headless run never
touches. See [DECISIONS.md](DECISIONS.md) D13.

It models the clocks that matter: 50 MHz card against a 2.8 MHz CPU, with `$Cxxx`
accesses stretched to 1.024 MHz.

**The card is clocked only while clocking it can change something.** Clocking
it 18 times per CPU cycle made a2min run at 0.4M simulated cycles per second,
7× slower than the machine it models. `perf` put essentially all of the time in
the Verilated model, which spends nearly every one of those cycles idle between
verbs. A Verilated model is deterministic, so once a full tick leaves every byte
of the model's state unchanged and writes nothing to the heap, every later tick
with the same inputs does the same. From then until the CPU next touches a card
register, cycles are counted, not simulated. This needs no knowledge of which
engines the RTL has, so it can't go stale as the RTL grows. foldlStrict at
`n` = 550 went from **84.7 s to 0.15 s** with byte-identical output.
`A2MIN_EXACT_CLOCK=1` simulates every cycle, and `just quiet-clock`
(`checks.quiet-clock`) runs every fixture and the M2 store
path both ways, requires identical output, then proves it can fail on a
deliberately broken copy.

It is **not** an Apple IIgs — no video, no ROM, no
soft switches, no interrupts — and unknown opcodes abort rather than being skipped,
because a skipped instruction would yield a wrong-but-plausible digest.

The 65816 model is itself under test, since the L4 VM runs on it:

```sh
just selftest          # 41 native-mode cases; expected values live in the harness
just mutation-a2min    # break the interpreter 20 ways, expect 20 catches
just bench-mul64-native # the ALU64 gate, measured in the mode the VM uses
```

The interpreter itself is `sim/cpu65816.h`, shared with `sim/orcarun.cpp` — the
harness that runs ORCA/C and the ORCA linker on this machine with no ROM and no
GS/OS ([TOOLCHAIN.md](TOOLCHAIN.md)). So the same 65816 model both compiles the
on-target C and runs it:

```sh
just orca-smoke        # compile, link and flatten target/tests/smoke.c; run it on a2min
orcabuild -o x.bin x.c # any C file, from the devShell
just orca-trace x.c    # the same compile with every OS/shell/Toolbox call printed
```

## What we are *not* doing

**The a2nix card is not the storage device.** That is the SmartPort storage card
rejected in [DECISIONS.md](DECISIONS.md) D1, and here it would also be circular: the
card would have to work in order to load the program that tests whether the card
works. Storage comes from MAME's own disk emulation, and the card stays a
compute/memory device.

## Route 1 — MAME debugger load (higher fidelity)

```sh
ROMPATH=/path/to/your/roms just m2-mame
```

MAME loads the raw binary straight into memory at `$2000` under debugger control, runs
it, and dumps the result. No OS, no disk image, no boot sequence — **M2 is a claim
about the card**, so involving ProDOS would add a dependency that can fail for reasons
unrelated to the thing under test.

This route buys fidelity that Route 0 cannot: a real 65816, the real FPI clock stretch,
real slot arbitration. It is worth running whenever you have a romset, but it is not
the gate, because it cannot run without one.

How the result gets out: the demo writes its 32 ASCII characters to `$0300` and a
status byte to `$0320`, *buffer first, status last*, so a reader that sees `$01` is
guaranteed to be looking at a complete answer. `scripts/run-m2.sh` compares that
against `tests/vectors/l1.txt` — which came from the real `nix` binary, not from
anything in this repo.

The headless variant is assembled with `-D NO_PRINT`, so printing is compiled out
entirely and the check never depends on the text screen or the monitor's state.

Moving parts:

| File | Role |
|---|---|
| `mame/debug/m2.txt` | debugger script: load, run, dump |
| `scripts/run-m2.sh` | drives MAME headlessly, compares against the vector |
| `just asm-headless` | builds `storepath_demo_headless.bin` |

## Route 2 — ProDOS disk image (interactive)

```sh
just disk
mame apple2gs -sl4 a2nix -flop3_5_1 target/build/a2nix.2mg \
    -autoboot_delay 4 -autoboot_command 'BRUN STOREPATH\n'
```

For watching the store path appear on a real text screen, and the route that matters
from L4 onward when there is an actual Nix binary to ship rather than a 400-byte demo.

`scripts/mkdisk.sh` builds the image with cadius. Two things to know:

- **cadius takes ProDOS type and auxtype from the filename**, as `NAME#TTAAAA`. The
  demo is staged as `STOREPATH#062000` — type `$06` (BIN), auxtype `$2000` (the load
  address). That auxtype must match `target/tests/apple2.cfg`, or `BRUN` loads to the
  wrong address and crashes.
- **cadius wants a sized volume string** (`800KB`, `32MB`); a bare block count is
  rejected.

The image holds no system files, so it is not bootable by itself. Either add
ProDOS/GS-OS yourself, or attach it alongside a system disk you already have.

## ROMs

The `apple2gs` ROM is copyrighted and not redistributable, so it is **user-supplied**:
this repository neither ships one nor says where to find one. Dump your own, or use a
set you already have, and give its directory **explicitly** as `ROMPATH` -- there is
no default location. `scripts/run-m2.sh` exits **77 (skipped)** rather than failing
when `ROMPATH` is unset or the set is incomplete, so CI stays honest about the
difference between "broken" and "cannot be tested here". The table below is how to
check a set: MAME verifies each file by CRC.

MAME 0.289 wants four files in `<rompath>/apple2gs.zip`:

| File | Size | CRC32 | Region |
|---|---:|---|---|
| `341-0728` | 131072 | `8d410067` | maincpu |
| `341-0748` | 131072 | `18190283` | maincpu |
| `341s0632-2.bin` | 4096 | `e1c11fb0` | adbmicro |
| `344s0047.bin` | 16384 | `2d541944` | **gfx1** (font) |

⚠️ **Sets made for MAME 0.278 and earlier** carry the superseded `megaii.chr`
(CRC `ef0bfc11`) in the `gfx1` slot rather than the later real dump `344s0047.bin`.
So a complete 0.278 set does **not** satisfy 0.289 — verify with
`mame -rompath <dir> -verifyroms apple2gs`.

Two ways round it if you want Route 1 without hunting for a newer dump: pin MAME to a
version matching your romset (the a2bus API differs — 0.281 and earlier use
`bus_reset()` rather than `reset_from_bus()`, and a non-const `take_c800()`), or patch
the `gfx1` `ROM_LOAD` to `ROM_LOAD_OPTIONAL` in our MAME override, since a headless run
never renders a glyph. Neither is needed for the gate.

GS/OS 6.0.x disk images are freely available and unrestricted, if you want Route 2
bootable — a System 6.0.1 *Install* disk is the useful one.

## A trap worth knowing

Scripts here prefix their environment variables (`A2_BIN`, `A2_VOLSIZE`, …). nixpkgs'
stdenv exports `SIZE`, `AR`, `NM`, `STRIP` and friends as binutils tool names, so a
plain `SIZE=${SIZE:-800KB}` silently inherits `"size"` inside `nix develop`.
