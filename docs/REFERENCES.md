# Verified references

Facts established by direct verification during design. **Do not re-derive these.**
If one turns out wrong, correct it here and note the impact.

## Apple IIgs timing — the design's keystone

- The IIgs runs at 2.8 MHz but **throttles to 1.024 MHz** for any access in
  `$C000–$CFFF`. The FPI dynamically stretches the clock to sync with the Mega II side,
  preserving compatibility for timing-sensitive I/O code.
  → one card byte ≈ **~10–11 fast-cycle equivalents**.
  <https://www.userlandia.com/home/iigs-mhz-myth>
- I/O space is reachable from banks `$E0`/`$E1` always, and from banks `$00`/`$01`
  under **bit 6 of the Shadow register**. Relevant to the L7 GS/OS driver.
- `LDA [dp],Y` — direct-page indirect **long** indexed, opcode `B7` — is **6 cycles**,
  **+1** if the direct-page register's low byte ≠ 0, **+1** on page cross.
  <https://sneslab.net/wiki/LDA>
- The 65816 has **no multiply and no divide instruction**. All multiplication is
  shift-add. Hence `ALU64`.

## 65816 toolchain

- `ca65` **fully supports the 65816**: `--cpu 65816` / `-p 816`, `.P816`, `.CPU 65816`,
  `.FARADDR` for 24-bit data, `--smart`/`.SMART` (tracks `REP`/`SEP` to auto-size
  operands), `.A8`/`.A16`/`.I8`/`.I16`. <https://cc65.github.io/doc/ca65.html>
- cc65's **C compiler is 6502-only**. There is no C or C++ backend for the 65816 in
  nixpkgs or upstream (`llvm-mos` is 6502 and absent from nixpkgs).

### C compilers for the 65816 (surveyed 2026-09-22 for D18)

- **No native-mode 65816 C compiler is open source.** LLVM-MOS has 24-bit
  addressing for the 65816 but 16-bit register mode is an open issue; Rust has no
  backend; cc65 is 6502-only. **Calypsi** (Linux, gratis, closed) and **WDC's**
  (Windows, closed) emit native-mode code. Declined to stay free of closed binaries.
- **ORCA/C** — <https://github.com/byteworksinc/ORCA-C>. Gratis; source on GitHub.
  LICENSE: "grants you the right to use this source code privately, fork it, and
  change it. You may not redistribute the code in any form other than submission
  to this repository" — **not** OSI-free, so `nix/pkgs/orcac.nix` marks it unfree
  and the flake allows that one name. `long long` since 2.2.0 (2023-07-30); C23
  features in 2.3.0 B1 (2026-07-13). Release assets are `.2mg` images:
  `ORCAC.230B1.2mg` = `sha256-6svoz7G+wJcg7CPo6D5AQvXw1kN+wHk/zgx3Z4ytRC4=`,
  `ORCAC.221.2mg` = `sha256-OCR/w5UCcQh880sxdmYspC2Gaczz4prB/ercyyizKHo=`. The
  image ships `Languages/cc`, `Languages/Linker`, `Libraries/{ORCALib,SysLib,
  SysFloat}`, `Libraries/ORCACDefs/*.h`, `ORCA.Sys16` and the release notes — the
  linker and libraries are in the free release, not only in Opus ][.
- Byte Works' other repositories (same licence): `Shell`, `Linker`, `ORCALib`,
  `SysLib`, `SysFloat`, `PasLib`, `DumpObj`, `MakeLib`. The shell and linker
  *sources* were how the harness's call surface was pinned down.
- **Golden Gate** (Kelvin Sherlock) runs ORCA tools on modern hosts by the same
  method; $10, closed, source only for buyers. <https://juiced.gs/store/golden-gate/>,
  manual <https://goldengate.gitlab.io/manual/> (prefix mapping: `13:` Libraries,
  `15:` Shell, `16:` Languages, `17:` Utilities; `iix compile ... keep=`).
- **GNO/ME** 2.0.6 is freeware with source (<https://github.com/GnoConsortium/gno>);
  it is the fallback route (ORCA/C under GNO under MAME), unused.

### The call surfaces the harness implements (sources)

- **ORCA shell calls:** numbers and parameter blocks are `ORCACDefs/shell.h`
  (class 0 `$0101`… and class 1 `$0141`…: GET_LINFO `$0101/$0141`, SET_LINFO
  `$0102/$0142`, VERSION `$0107/$0147`, FASTFILE `$010E/$014E` …). Semantics:
  `Shell/call.asm` (`GetLInfo`, `FastFile` actions 0 Load, 1 IndexedLoad,
  2 LoadMemory, 3 Save, 4 AddFile, 5 Delete, 6 Remove, 7 Purge, 8 SetInfo; flags
  `$8000` purge-on-Purge…). How the shell starts a program: `Shell/cmd.asm`
  `Call` — `InitialLoad2`, a 4 KB default stack, DP = stack base, A = user ID,
  X:Y = the command line prefixed by `BYTEWRKS`, `JSL` to the entry.
- **GS/OS calls:** `ORCACDefs/gsos.h` (numbers and records). Both the inline form
  (`JSL $E100A8`, `.word num`, `.long pb`) and the stack form (`$E100B0`) are used.
- **Toolbox call numbers:** the `inline(0xNNNN, dispatcher)` declarations in
  `ORCACDefs/{locator,memory,misctool,intmath,texttool}.h`; the Pascal runtime
  (`PasLib/nl.asm`) reaches the console through SysLib's `SysCharOut`, which is a
  GS/OS `WriteGS` on the refnum from `GetStdRefNum`.
- **OMF:** headers per `DumpObj.cc` `GetHead`; **SUPER records** ($F7) per the
  Linker's `out.asm` and 6502bench/CiderPress2 `OmfReloc.cs`: type 0 = RELOC2,
  1 = RELOC3, 2–13 = INTERSEG1–12 (3-byte, segment in the patched data), 14–25 =
  INTERSEG13–24 (2-byte), 26–37 = INTERSEG25–36 (2-byte, shift −16); body =
  page-skip bytes (`$80|n`) and per-page patch counts. **Jump table** entries
  (Linker `out.asm` `JumpTable`): userID, file, segment, offset, `JSL` loader —
  14 bytes after an 8-byte header. **ExpressLoad** (Neil Parker,
  <http://nparker.llx.com/a2/loader.html>): a dynamic data segment named
  `~ExpressLoad`, first in the file, skipped by the harness.
- **ORCA/C's own OS use** (`CC.pas`, `CCommon.pas`): VERSION must report a
  version byte ≥ `'2'` (ASCII), GET_LINFO with 11 parameters, FastFile to read
  the source and write `.root`/`.a`, `-i` skips the `.sym` file, `-p` silences
  progress; flag letters map `a` → bit 31 … `z` → bit 6 (`flag_d = $10000000`).

## MAME card interface

`device_a2bus_card_interface`, verified against tag **`mame0289`** — the version the
flake actually pins (`src/devices/bus/a2bus/a2bus.h`):

```cpp
// C0nX (/DEVSEL)
virtual uint8_t read_c0nx(uint8_t offset)
virtual void    write_c0nx(uint8_t offset, uint8_t data)
// CnXX (/IOSEL)
virtual uint8_t read_cnxx(uint8_t offset)
virtual void    write_cnxx(uint8_t offset, uint8_t data)
// C800 (/IOSTB)
virtual uint8_t read_c800(uint16_t offset)
virtual void    write_c800(uint16_t offset, uint8_t data)
virtual bool    take_c800() const            // NOTE: const
// Inhibit
virtual uint8_t read_inh_rom(uint16_t offset)
virtual void    write_inh_rom(uint16_t offset, uint8_t data)
virtual uint16_t inh_start() / inh_end() / inh_check(...) / inh_type()
// Reset
virtual void    reset_from_bus()             // NOTE: renamed

// protected helpers
void     raise_slot_irq() / lower_slot_irq()
void     raise_slot_nmi() / lower_slot_nmi()
void     raise_slot_dma() / lower_slot_dma()
void     recalc_slot_inh()
uint8_t  slot_dma_read(uint16_t offset)
void     slot_dma_write(uint16_t offset, uint8_t data)
uint32_t get_slotromspace() / get_slotiospace()

device_a2bus_card_interface(const machine_config &mconfig, device_t &device)
int slotno() const;  a2bus_device &a2bus();
```

No pure virtuals — every method has a default, so a card only overrides what it uses.

**Changes from 0.281** (the risk this note existed to catch, and it fired):

| 0.281 | 0.289 |
|---|---|
| `bus_reset()` | **`reset_from_bus()`** — renamed |
| `take_c800()` | **`take_c800() const`** — now const |
| — | `recalc_slot_inh()`, `slot_dma_read/write()` added |

This still maps ~1:1 onto a Verilog port list, which is why RTL-in-MAME works without
an abstraction layer. **DMA and interrupts are both available**, and `slot_dma_read/
write` make bus-master DMA straightforward — relevant to L7.

**Where to patch.** Cards are registered in `src/devices/bus/a2bus/cards.cpp`, which
defines `apple2_slot0_cards()`, `apple2_cards()`, `apple2e_cards()`,
`apple2gs_cards()` and `apple3_cards()`. Each adds entries as:

```cpp
device.option_add("mockingboard", A2BUS_MOCKINGBOARD);
device.option_add("scsi",         A2BUS_SCSI);
```

So the L2 patch is: include our header in `cards.cpp`, add one `option_add` line to
`apple2gs_cards()`, and add the source files to `scripts/src/bus.lua`.

## Nix formats

**Store path** — `type ":sha256:" inner-digest ":" store ":" name` → SHA-256 →
compress to 160 bits → base-32. Base-32 processes the byte string **from the end**
(base-16 processes from the beginning).
<https://nix.dev/manual/nix/2.28/protocols/store-path>

- **nixbase32 alphabet:** `0123456789abcdfghijklmnpqrsvwxyz` — digits plus lowercase
  letters **excluding `e`, `o`, `u`, `t`**. 32 characters, all filename-safe.
- **XOR fold:** literally `result[i % 20] ^= h[i]` over the 32 input bytes.
- **Why 160 bits:** SHA-1 was 160 bits and encoded to exactly 32 base32 characters.
  When SHA-256 became necessary in 2017, the 160-bit truncation was retained for
  backwards compatibility with the 32-character path width.

**NAR** — `str("nix-archive-1")` then nested structures, where
`str(s) = int(|s|) + pad(s)`, `int(n)` is a **64-bit little-endian** length, and
`pad(s)` pads with zeros to a multiple of 8 bytes. Deterministic: sorted entries, no
timestamps, no padding ambiguity. The 65816 is little-endian, so this parses naturally.
<https://nix.dev/manual/nix/2.22/protocols/nix-archive>

**Derivations** are stored on disk in **ATerm** format. The ATerm text does *not*
contain the derivation's name — that arrives out-of-band via the store path.
<https://nix.dev/manual/nix/2.28/protocols/derivation-aterm>

**ProDOS block device signature** (documented for reference; not used before L7) —
`$Cn01=$20`, `$Cn03=$00`, `$Cn05=$03`; `$CnFF` = low byte of the driver entry;
`$CnFE` = status byte, where bits 0 and 1 must be set for ProDOS to install the driver
vector. ProDOS driver params live at `$42` (command), `$43` (unit), `$44-45` (buffer),
`$46-47` (block). Errors: `$27` I/O, `$28` no device, `$2B` write protected.

## Prior art

- **Reduceron** — FPGA softcore for lazy functional programs with hardware GC, 60–150
  MHz, built in York Lava (a Haskell HDL) with the F-lite compiler. Memos of interest:
  **28** "Mark-Compact garbage collection in O(survivors) time", **27** "Design of the
  Octostack", **25** template instantiation, **7** memory layout, **31** primitive
  redexes. <https://mn416.github.io/reduceron-project/> ·
  <https://github.com/tommythorn/Reduceron>
- **Lineage** — PilGRIM (processor for lazy functional languages), Massimult
  (arXiv 2412.02765, Dec 2024; LambdaM / KVY / Matrima, structured combinators),
  KappaMutor.
- **Lisp machines** — Symbolics 3600 used 36-bit words with 4 permanent tag bits;
  **CDR-coding** compressed list cells to roughly half the space and reportedly
  improved GC by an order of magnitude. The CADR had hardware type testing, CDR
  coding, and incremental GC. Symbolics' GC combined incremental copying,
  approximately-depth-first copying, and **ephemeral objects** — i.e. generational.
  Canonical: Moon, *Garbage Collection in a Large Lisp System* (1984).
- **Region tagging** — determine a value's type from which memory region holds it
  rather than from tag bits; used by early 6502 Lisps.
  <http://forum.6502.org/viewtopic.php?t=1427>
- **tvix** — Rust Nix implementation; a **bytecode VM** (compiles the rnix AST to
  bytecode). Its conformance corpus is split into `eval-okay`, `eval-fail`,
  `parse-okay`, `parse-fail`, each with a `notyetpassing` directory for expected
  failures that get promoted as compatibility improves. <https://github.com/tvlfyi/tvix/>
- **Marinetti** — the only TCP/IP stack for the Apple IIgs, clean-roomed from the RFCs
  and written **entirely in 65c816 assembly**; an Uthernet II Marinetti link-layer
  driver exists, as does a bootable GS/OS starter image.
  <https://a2retrosystems.com/Marinetti.htm>
- **a2retronet / a2pico** (both MIT) — RP2040 Apple II card platform; PIO latches
  A0–A11 and R/W on ENBL falling edge, with ARM core 1 running uninterrupted from RAM
  to meet the ~300 ns write window. Reference for real-hardware timing at L7+.
  <https://github.com/oliverschmidt/a2retronet>

## GS/OS documentation (L7)

FST and driver work is **documented, not reverse-engineered** — this risk was
downgraded during design.

- *Apple IIgs GS/OS Internals* — covers FST programming and the FST system entry
  routine. <https://www.brutaldeluxe.fr/documentation/gsos/Apple_IIgs_GSOS_internals_v1.pdf>
- *GS/OS Reference, Volume 1* — "Writing a Device Driver", device calls including
  `DStatus`/`DControl`, and the System Loader.
  <http://www.apple-iigs.info/doc/fichiers/gsosref1.pdf>

## nixpkgs availability

**As pinned by `flake.lock` (nixos-unstable), which is what actually builds:**
`verilator` **5.052**, `yosys` **0.69**, `mame` **0.289**, `ca65` **2.18**,
`surfer` 0.7.0, plus `iverilog`, `nextpnr`, `trellis`, `icestorm`, `openfpgaloader`,
`verible`, `gtkwave`, `just`, and `python313Packages.cocotb` 2.0.0.

Two traps found the hard way:

- **cocotb 2.x does not support Python 3.14**, which is the current `python3` default.
  The devShell pins `python313`.
- **verilator needs `python3` at build time** for its own `verilator_includer`, so any
  derivation invoking `verilator --build` must put `python3` in `nativeBuildInputs`.

⚠️ **`mame` is 0.289 here, but the `a2bus` interface above was verified against tag
`mame0281`.** Re-verify the method signatures against the pinned version before
writing the L2 patch — MAME's internal APIs are not stable across releases.

*(An earlier survey of the local channel reported verilator 5.040 / yosys 0.55 /
mame 0.281 / cc65 2.19. Those were channel versions, not the flake's, and the flake
is authoritative.)*

**Absent, must be packaged:** `applecommander` (Java jar; `java` is present),
`cadius`, `a2tools`, `diskm8`, `ciderpress`, `nulib2`, `llvm-mos`, `apicula`,
`ecpprog`, `prjoxide`. No Apple II emulator other than MAME is packaged.
