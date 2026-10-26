# Where everything external comes from

This repository builds on things it does not contain. Some it may not contain:
ORCA/C may not be redistributed, and the Apple IIgs ROMs are copyrighted. This
page records every one, pinned by hash, with the search terms to find it again
if a link dies. Nix only checks the hash of a fixed-output download, never its
URL, so a copy obtained anywhere satisfies the build. Add a flat file with
`nix-store --add-fixed sha256 <file>`, or point the derivation's `url` at
`file:///path/to/copy`.

For source repositories, [Software Heritage](https://archive.softwareheritage.org/)
keeps git history by commit hash. For release files, search the
[Internet Archive](https://archive.org/) and the Asimov Apple II archive
(`ftp.apple.asimov.net`, mirrored at `mirrors.apple2.org.za`) with the terms
given.

## ORCA/C 2.3.0 B1: the C compiler (unfree, fetched, never redistributed)

The on-target VM is C, compiled by ORCA/C under this repo's own harness
(TOOLCHAIN.md, D18). Byte Works publishes it gratis; its licence permits private
use, forking and modification but **not redistribution**. So `nix/pkgs/orcac.nix`
marks it unfree and not redistributable, the flake allows exactly that package by
name, and every machine downloads it itself.

| | |
|---|---|
| Release image | <https://github.com/byteworksinc/ORCA-C/releases/download/orcac-230b1/ORCAC.230B1.2mg> |
| Hash (flat file) | `sha256-6svoz7G+wJcg7CPo6D5AQvXw1kN+wHk/zgx3Z4ytRC4=` |
| Headers the image lacks | <https://github.com/byteworksinc/ORCA-C>, tag `orcac-230b1`, commit `53342ce6faa0f8f15a891ea44933f52d73a89457`, `sha256-FgNC0EqtU3WGC0BRcrL4Sqx14nyP5Wh7K99sHGG0sCY=` (unpacked) |
| Licence | <https://github.com/byteworksinc/ORCA-C/blob/main/LICENSE> |
| Search terms | `ORCA/C`, `ORCAC.230B1.2mg`, `ORCA/C 2.3.0`, `Byte Works ORCA`, `Opus ][ The Software` (Byte Works' collection of the ORCA languages) |

Two consequences to keep:

- **Never push the built ORCA tree to a public binary cache**, such as a public
  Cachix. That would be redistribution. cache.nixos.org never builds unfree
  packages.
- **Binaries compiled with it** (`target/build/cvm.bin`) link Byte Works'
  runtime libraries (ORCALib, SysLib). None is committed (`target/build/` is
  ignored). Check what Byte Works permits for linked runtimes before
  publishing one.

## Apple IIgs ROMs: user-supplied, explicitly

Copyrighted, and not redistributable. This repository neither ships them nor
says where to find them. Only the optional MAME route (RUNNING.md, Route 1)
needs them, and you give their directory as `ROMPATH`. There is no default,
and without it the check skips (exit 77). Dump your own, or use a set you
have. RUNNING.md "ROMs" lists the four files and their CRCs, which MAME
verifies (`mame -rompath <dir> -verifyroms apple2gs`). Nothing the gates need
is ROM-dependent: M2 runs on a2min, which needs no ROM at all (D13).

## GS/OS System 6.0.1 (L7, not yet used)

Apple made System 6.0.1 a free download. It is for booting Route 2
(RUNNING.md) and for the file-system layer (L7). Nothing fetches it yet.
Search terms: `Apple IIgs System 6.0.1`, `GS/OS 6.0.1`, `System 6.0.1 Install
disk`.

## Disk-image tools (free, fetched)

| Tool | Source | Hash | Search terms |
|---|---|---|---|
| cadius 1.4.5 | <https://github.com/mach-kernel/cadius/archive/refs/tags/1.4.5.tar.gz> | `sha256-s7fq5dwJCIeFgouCvJIKu41ir3IyiLt3UWcqjLHXaJc=` (unpacked) | `cadius Brutal Deluxe`, `mach-kernel cadius` |
| AppleCommander 14.1 | <https://github.com/AppleCommander/AppleCommander/releases/download/14.1/AppleCommander-ac-14.1.jar> | `sha256-/oK57R6eFbJYY7w14lAV2nzjAQexdityddRSjZDmmM4=` | `AppleCommander` |

## Everything else in the devShell

From nixpkgs, pinned by `flake.lock`: `NixOS/nixpkgs` rev
`44a91898084f46797b5fac650c7e8c9ac38c43d4`. That covers Verilator, yosys,
nextpnr, trellis, cc65, cocotb, Rust, and MAME 0.289, which
`nix/pkgs/mame-a2nix.nix` overrides with the a2nix card. The generator's one
crate is `rnix` 0.14.0 (MIT), pinned in `tools/nbc/Cargo.lock`.

## Test data and Nix itself

| | |
|---|---|
| The scoreboard corpus | tvix, `eval/src/tests/`, commit `9fbca101331eecbf5c1cd44861f3d24aff101b82`: <https://github.com/tvlfyi/tvix> (mirror of `code.tvl.fyi/depot`), GPL-3.0, vendored with its licence in `tests/corpus/tvix/` |
| `derivation.nix` | Nix 2.34.8, `src/libexpr/primops/derivation.nix`, LGPL-2.1, embedded in `tools/nbc/src/derivation.nix`: <https://github.com/NixOS/nix> |
| Ground truth | Every `.exp` is the output of the host's `nix-instantiate --eval --strict` (Nix 2.34.8; the fixed-output fixtures, 2.34.9, whose source -- `github:NixOS/nix/2.34.9`, `sha256-ZJwg/qG4M1Impy774HKMZ3Hsq7vlBJy7sXl1fI9QDXI=` -- the rules in DERIVATION.md "Fixed outputs" were read from), and every store-path vector came from the real `nix` binary (D7) |
