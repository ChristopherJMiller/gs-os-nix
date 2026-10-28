# SPDX-License-Identifier: GPL-3.0-or-later
"""Reader for the KiCad s-expression netlist that SKiDL's generate_netlist() writes.

Every gate that asks "what is connected to what" reads the netlist through this module,
so connectivity always comes from the pads and never from names written on tracks.
"""
import re
from dataclasses import dataclass, field
from pathlib import Path

_TOKEN = re.compile(r'\s*(?:(\()|(\))|"((?:[^"\\]|\\.)*)"|([^\s()"]+))')


@dataclass(frozen=True)
class PartInfo:
    value: str
    footprint: str
    fields: dict[str, str]


@dataclass
class Netlist:
    parts: dict[str, PartInfo] = field(default_factory=dict)
    nets: dict[str, set[tuple[str, str]]] = field(default_factory=dict)


def _parse(text: str) -> list:
    """Parse one s-expression into nested lists of str (quoted and bare atoms alike)."""
    stack: list[list] = [[]]
    pos = 0
    while True:
        m = _TOKEN.match(text, pos)
        if not m:
            if text[pos:].strip():
                raise ValueError(f"netlist: cannot parse at offset {pos}")
            break
        pos = m.end()
        if m.group(1):
            stack.append([])
        elif m.group(2):
            if len(stack) < 2:
                raise ValueError("netlist: unbalanced ')'")
            done = stack.pop()
            stack[-1].append(done)
        elif m.group(3) is not None:
            stack[-1].append(re.sub(r"\\(.)", r"\1", m.group(3)))
        else:
            stack[-1].append(m.group(4))
    if len(stack) != 1 or len(stack[0]) != 1:
        raise ValueError("netlist: unbalanced '(' or not a single expression")
    return stack[0][0]


def _kids(node: list, head: str) -> list[list]:
    return [c for c in node if isinstance(c, list) and c and c[0] == head]


def _text(node: list, head: str) -> str:
    for c in _kids(node, head):
        return c[1] if len(c) > 1 and isinstance(c[1], str) else ""
    return ""


def load(path: Path) -> Netlist:
    root = _parse(Path(path).read_text())
    nl = Netlist()
    for comps in _kids(root, "components"):
        for comp in _kids(comps, "comp"):
            fields: dict[str, str] = {}
            for fs in _kids(comp, "fields"):
                for f in _kids(fs, "field"):
                    name = _text(f, "name")
                    rest = [x for x in f[1:] if isinstance(x, str)]
                    fields[name] = rest[0] if rest else ""
            nl.parts[_text(comp, "ref")] = PartInfo(
                _text(comp, "value"), _text(comp, "footprint"), fields)
    for nets in _kids(root, "nets"):
        for net in _kids(nets, "net"):
            nodes = nl.nets.setdefault(_text(net, "name"), set())
            for node in _kids(net, "node"):
                nodes.add((_text(node, "ref"), _text(node, "pin")))
    return nl


def net_of(nl: Netlist, ref: str, pin: str) -> str | None:
    for name, nodes in nl.nets.items():
        if (ref, pin) in nodes:
            return name
    return None


def neighbours(nl: Netlist, net: str) -> set[str]:
    return {ref for ref, _ in nl.nets.get(net, set())}


def through(nl: Netlist, ref: str, net: str) -> str:
    """The net on the other pin of 2-pin part `ref`, which has one pin on `net`."""
    pin_net = {pin: n for n, nodes in nl.nets.items() for r, pin in nodes if r == ref}
    if len(pin_net) != 2:
        raise ValueError(f"{ref} is not a 2-pin part ({len(pin_net)} connected pins)")
    on = [p for p, n in pin_net.items() if n == net]
    if not on:
        raise ValueError(f"{ref} has no pin on net {net}")
    other = next(p for p in pin_net if p != on[0])
    return pin_net[other]
