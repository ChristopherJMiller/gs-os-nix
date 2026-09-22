# gs-os-nix

Running **Nix on an Apple IIgs** under GS/OS, with a custom FPGA accelerator card
supplying the three things a 1986 machine lacks: memory capacity, hashing throughput,
and pointer/search throughput.

The goal is not a tribute. It is an Apple IIgs that computes store paths and `.drv`
hashes **byte-identically to upstream Nix**, evaluating real Nix natively.

## Orientation

| Doc | Read it for |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | The machine: what runs where, and why |
| [docs/CARD-ISA.md](docs/CARD-ISA.md) | **Normative** card interface — register map and verbs |
| [docs/HEAP.md](docs/HEAP.md) | **Normative** heap object layout and tags |
| [docs/DECISIONS.md](docs/DECISIONS.md) | Why it's built this way, and what was rejected |
| [docs/REFERENCES.md](docs/REFERENCES.md) | Verified facts with citations — don't re-derive these |
| [docs/RUNNING.md](docs/RUNNING.md) | Getting code onto the emulated IIgs |
| [docs/STATUS.md](docs/STATUS.md) | Layer progress and how to resume |

## The one-paragraph version

The Apple IIgs owns everything semantic: lexer, parser, compiler, bytecode VM, lazy
forcing, all primops, string context, the store, derivation logic, GS/OS integration.
The card owns memory capacity and compute-bound kernels, and **never makes a decision
or touches the world**. Linux does nothing at runtime — it only builds the toolchain,
generates golden test fixtures, and runs MAME for CI.

The design's keystone: the IIgs throttles to 1.024 MHz for any `$Cxxx` access, making
one card access cost about the same as one fast-RAM pointer dereference. Therefore
**the bus carries verbs, not data** — and the generational GC boundary *is* the slot
connector, with the nursery in fast RAM and mature space on the card.

## Quick start

```sh
nix develop          # verilator, cocotb, yosys, nextpnr, trellis, cc65, mame
nix flake check      # cocotb suites, lint, synth smoke, conformance scoreboard
```

`apple2gs` ROMs are not redistributable and must be supplied by you; MAME-dependent
checks skip gracefully without them.
