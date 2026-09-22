# Getting code onto the emulated Apple IIgs

Two routes, for two different jobs. Both need `nix develop`.

## What we are *not* doing

**The a2nix card is not the storage device.** That is the SmartPort storage card
rejected in [DECISIONS.md](DECISIONS.md) D1, and here it would also be circular: the
card would have to work in order to load the program that tests whether the card
works. Storage comes from MAME's own disk emulation, and the card stays a
compute/memory device.

## Route 1 — debugger load (automated, for M2 and CI)

```sh
ROMPATH=~/mame/roms just m2
```

The fastest and most deterministic path: MAME loads the raw binary straight into
memory at `$2000` under debugger control, runs it, and dumps the result. No OS, no
disk image, no boot sequence.

That is a deliberate choice rather than a shortcut. **M2 is a claim about the card**,
so involving ProDOS would add a dependency that can fail for reasons unrelated to the
thing under test.

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

The `apple2gs` ROM is copyrighted and not redistributable, so it is **user-supplied**.
Point MAME at yours with `ROMPATH`. `scripts/run-m2.sh` exits **77 (skipped)** rather
than failing when the ROMs are missing, so CI stays honest about the difference
between "broken" and "cannot be tested here".

GS/OS 6.0.x disk images are freely available if you want route 2 bootable.

## A trap worth knowing

Scripts here prefix their environment variables (`A2_BIN`, `A2_VOLSIZE`, …). nixpkgs'
stdenv exports `SIZE`, `AR`, `NM`, `STRIP` and friends as binutils tool names, so a
plain `SIZE=${SIZE:-800KB}` silently inherits `"size"` inside `nix develop`.
