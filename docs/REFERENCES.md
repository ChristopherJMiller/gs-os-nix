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

## MAME card interface

`device_a2bus_card_interface`, verified against tag `mame0281`
(`src/devices/bus/a2bus/a2bus.h`):

```cpp
virtual uint8_t read_c0nx(uint8_t offset)
virtual void    write_c0nx(uint8_t offset, uint8_t data)
virtual uint8_t read_cnxx(uint8_t offset)
virtual void    write_cnxx(uint8_t offset, uint8_t data)
virtual uint8_t read_c800(uint16_t offset)
virtual void    write_c800(uint16_t offset, uint8_t data)
virtual bool    take_c800()
virtual uint8_t read_inh_rom(uint16_t offset)
virtual void    write_inh_rom(uint16_t offset, uint8_t data)
virtual uint16_t inh_start()  /  inh_end()  /  inh_check(...)  /  inh_type()
virtual void    bus_reset()
void raise_slot_irq() / lower_slot_irq()
void raise_slot_nmi() / lower_slot_nmi()
void raise_slot_dma() / lower_slot_dma()
```

This maps ~1:1 onto a Verilog port list, which is why RTL-in-MAME works without an
abstraction layer. Note that **DMA and interrupts are both available** — relevant to
L7 stretch work.

nixpkgs `mame` is **0.281, built from source** (GitHub archive `mame0281`, 1 patch), so
`overrideAttrs` can inject a new device. Adding a card requires editing
`scripts/src/bus.lua` and the a2bus slot-option registration.

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
