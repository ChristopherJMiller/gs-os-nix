# SPDX-License-Identifier: GPL-3.0-or-later
"""pins.toml: the FPGA's port -> ball assignment, and the LPF generated from it."""
import re
import tomllib
from dataclasses import dataclass
from pathlib import Path
from typing import Literal


@dataclass
class Port:
    name: str
    dir: Literal["in", "out", "inout"]
    balls: list[str]
    io_standard: str
    on_module: bool


@dataclass
class PinMap:
    part: str
    ports: dict[str, Port]


def load(path: Path) -> PinMap:
    data = tomllib.loads(Path(path).read_text())
    default_std = data.get("io_standard", "LVCMOS33")
    ports: dict[str, Port] = {}
    owner: dict[str, str] = {}
    for name, p in data.get("ports", {}).items():
        if p["dir"] not in ("in", "out", "inout"):
            raise ValueError(f"port {name}: bad dir {p['dir']!r}")
        for ball in p["balls"]:
            if ball in owner:
                raise ValueError(f"ball {ball} assigned to both {owner[ball]} and {name}")
            owner[ball] = name
        ports[name] = Port(name, p["dir"], list(p["balls"]),
                           p.get("io_standard", default_std), p.get("on_module", False))
    return PinMap(data["part"], ports)


def bits(p: Port) -> list[tuple[str, str, str]]:
    """(net name, LPF name, ball) per bit; a 1-ball port is a scalar."""
    if len(p.balls) == 1:
        return [(p.name, p.name, p.balls[0])]
    return [(f"{p.name}{i}", f"{p.name}[{i}]", b) for i, b in enumerate(p.balls)]


def to_lpf(pm: PinMap) -> str:
    out = ["# Generated from pins.toml -- do not edit."]
    for p in pm.ports.values():
        for _, lpf, ball in bits(p):
            out.append(f'LOCATE COMP "{lpf}" SITE "{ball}";')
            out.append(f'IOBUF PORT "{lpf}" IO_TYPE={p.io_standard};')
    return "\n".join(out) + "\n"


_COMMENTS = re.compile(r"//[^\n]*|/\*.*?\*/", re.S)
_DECL = re.compile(r"(input|output|inout)\b\s*(?:(?:wire|reg|logic)\b\s*)?"
                   r"(?:\[\s*(\d+)\s*:\s*(\d+)\s*\]\s*)?(\w+)$")


def verilog_ports(path: Path, module: str) -> dict[str, tuple[str, int]]:
    """Port -> (direction, width) from the ANSI port list of `module`."""
    text = _COMMENTS.sub(" ", Path(path).read_text())
    m = re.search(rf"\bmodule\s+{re.escape(module)}\b[^(]*\(", text)
    if not m:
        raise ValueError(f"module {module} not found in {path}")
    depth, i = 1, m.end()
    while depth and i < len(text):
        depth += (text[i] == "(") - (text[i] == ")")
        i += 1
    ports: dict[str, tuple[str, int]] = {}
    cur = None  # (direction, width) carried to bare names after a comma
    for item in text[m.end():i - 1].split(","):
        item = item.strip()
        if not item:
            continue
        d = _DECL.match(item)
        if d:
            direction, hi, lo, name = d.groups()
            cur = (direction, abs(int(hi) - int(lo)) + 1 if hi is not None else 1)
        elif cur and re.fullmatch(r"\w+", item):
            name = item
        else:
            raise ValueError(f"cannot parse port declaration {item!r}")
        ports[name] = cur
    return ports
