# The C toolchain: ORCA/C under our own harness (D18)

## Shape

```
  foo.c ──> orcacc ──> foo.root, foo.a ──> orcalink ──> foo (OMF load file) ──> omf2bin ──> foo.bin
             │                              │                                     │
             │  ORCA/C's `cc`               │  ORCA's `Linker`                    │  host code
             └──── inside orcarun ──────────┘  inside orcarun                     └─ flat, at $2000

  orcabuild -o foo.bin foo.c        all three; mkOrcaBinary { name; srcs; } in the flake
```

`sim/orcarun.cpp` is the 65816 core shared with a2min (`sim/cpu65816.h`) wrapped in
a different world: instead of RAM, a slot decoder and the card, it provides what an
ORCA shell program sees — GS/OS, the ORCA shell and the Toolbox — implemented on
the host. That is the approach Golden Gate takes; this is a free re-implementation
of the subset ORCA's own tools use, so it needs **no ROM and no GS/OS** and runs in
`nix flake check` like everything else. `checks.orcac-smoke` is the proof:
`target/tests/smoke.c`, compiled and linked by the real ORCA binaries, runs on
a2min and leaves the hand-computed bytes in the result area.

## What the harness provides

Grown against the real compiler and linker with one rule: a call the harness does
not implement **aborts naming the call**. A missing call must be an error, never a
silent no-op — a compiler that "succeeds" over an unimplemented OS call produces a
wrong-but-plausible object file, the failure mode this project is most careful
about. Everything below exists because one of the two programs called it.

| Layer | Entry | What ORCA/C and the linker use |
|---|---|---|
| **GS/OS** | `JSL $E100A8` + inline call number and parameter-block pointer; `JSL $E100B0` stack form | class 1: Create, Destroy, Get/SetFileInfo, Set/GetPrefix, ExpandPath, Open (with the file-info fields), Newline, Read, Write, Close, Flush, Set/GetMark, Set/GetEOF, GetDirEntry, GetRefInfo, GetStdRefNum, GetName, GetVersion, Quit; the class-0 (ProDOS 16, P-string) forms of the same |
| **ORCA shell** | the same entry, call numbers `$01xx` (class 0) and `$014x` (class 1) | GET/SET_LINFO (the command line the shell would build: source, keep name, `-`/`+` flags, `lops`, `merr`), FASTFILE (Load/Save/Purge… — how ORCA/C reads its source and writes its objects), READ_VARIABLE, SET, VERSION, STOP, DIRECTION, EXPAND_DEVICES, WRITE_CONSOLE |
| **Toolbox** | `JSL $E10000`, `X` = call, Pascal stack frame | Tool Locator (stubs), **Memory Manager** (real 20-byte handle records in bank `$E0`, so `lda [h]` works; NewHandle attributes honoured: locked/fixed/page-aligned/bank-0/no-bank-cross; SetHandleSize moves when it must), Misc Tools (a fixed clock: builds are deterministic), Integer Math, Text Tools (console → stdout/stderr), SANE start-up stubs |
| **Loader** | OMF v1/v2 load files | every segment placed by the Memory Manager; ExpressLoad segments skipped; CONST/LCONST/DS/ALIGN; RELOC, cRELOC, INTERSEG, cINTERSEG and all 38 **SUPER** record types (the Linker's compact form); jump tables for dynamic segments patched to `JML` since every segment is loaded eagerly |

Not provided, on purpose: interrupts, the desktop, SANE arithmetic (ORCA/C only
starts it), EXECUTE (the harness runs one program), redirection to files.

### The filesystem

The host's. GS/OS prefixes 0–31 map to host directories (`--map N=DIR`; `--orca ROOT`
sets 13 = `Libraries`, 15, 16 = `Languages`, 17, `*` and `@`); a full pathname
`:H:a:b` is host `/a/b`. Names match case-insensitively (ProDOS semantics). A file's
ProDOS type and auxtype ride on its host name as cadius writes them — `cc#B5DB01`
is an EXE with auxtype `$DB01` — and files the programs create get the same
suffix; files without one are typed by extension (`.c` → SRC/C, `.root`/`.a` → OBJ).
The wrapper commands strip the suffixes on the way out. ORCA/C's source files are
CR-terminated; `orcacc` converts on the way in.

## Distribution

`nix/pkgs/orcac.nix` fetches the official 2.3.0 B1 release image from
byteworksinc/ORCA-C (pinned by hash) and extracts it with cadius. Its licence
allows private use, forking and modification but not redistribution, so it is
marked unfree and the flake's nixpkgs allows exactly that one package by name; the
flake redistributes nothing — every machine fetches the release itself. The
compiler, linker, `ORCALib`, `SysLib`, `SysFloat` and the headers all ship in it.

## Commands

```sh
orcacc   [-o NAME] [-I DIR]... [-O] [-d OUTDIR] FILE.c    # -> NAME.root, NAME.a
orcalink [-o OUT] [-d OUTDIR] NAME...                      # -> OUT, an OMF load file
omf2bin  [--org N] IN OUT                                  # -> flat binary for a2min
orcabuild -o OUT.bin [-O] [-I DIR]... FILE.c...            # all three
orcarun  --trace ...                                       # every OS/shell/Toolbox call
just orca-smoke                                            # the smoke test, by hand
```

`omf2bin` places the load file's first segment (code and, in the small memory
model, every global) page-aligned from `--org` (default `$2000`, where a2min
loads) and prefixes a 23-byte stub: enter native mode, save the emulation stack
pointer, give the C code a stack at `$1FFF`, `JSL` the entry, restore, `RTS` to
a2min's halt. Bank 0 ends at `$C000` — the IIgs's I/O space — so a program that
outgrows it puts code in load segments of its own (ORCA/C's `segment "name";`),
which `omf2bin` places in banks 4 and up; ORCA/C calls every function with `JSL`,
and absolute addressing still means bank 0 because nothing changes the data bank
register. Such an image is written in a chunked form — `GSIM`, a count, then
(address, length, bytes) per chunk — which a2min recognises by its first bytes;
a program that fits in bank 0 is still a flat image. A program built this way has
no ORCALib start-up and no OS — `#pragma noroot`, an entry function first in the
first file (the load file's entry point is the first function of the first
object), results written to the a2min result area. That is what the VM needs.

## Memory model for our code

ORCA/C's large memory model: `int` 16 bits, `long` 32, `long long` 64 (Nix
integers; in SysLib since 2.2.0), pointers 32 bits (24 used). Locals live in the
direct page (`tsc / sbc #frame / tcs / phd / tcd` on entry), so the DP register
moves per call. The VM's heap layout (HEAP.md) is unchanged; the C is written
against it as `struct`s with explicit little-endian fields. The dispatch loop,
`FORCE`, and the card I/O may stay as inline `asm { }`.

## Gates

- **`checks.orcac-smoke`** — compile, link, convert, run; assert the result bytes.
- **`checks.orcarun`** — the harness package builds.
- **`checks.a2min-native` + `checks.mutation-a2min`** now also cover the core ORCA/C
  itself runs on: a wrong 65816 would miscompile the VM *and* misrun it.
- Everything that exists for the VM — fixtures, corpus scoreboard, GC-stress —
  verifies the C port against the assembly one; the assembly VM stays in the tree
  until the C VM passes every test it does.

## Follow-up (STATUS.md)

A black-box re-implementation of a FOSS native-mode 65816 C compiler — or landing
16-bit register mode in LLVM-MOS — removes the last non-free tool. Not started.
