# SPDX-License-Identifier: GPL-3.0-or-later
"""The iCESugar-Pro module's SODIMM-200 pin map (hw/carrier/icesugar_pro_v1_3.toml)."""
import tomllib
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class ModuleMap:
    rails: dict[str, list[int]]
    rail_role: dict[str, str]
    balls: dict[int, str]
    bank_vccio: dict[int, float]
    pio: dict[int, str] = field(default_factory=dict)  # pin -> schematic PIO label
    notes: dict[str, str] = field(default_factory=dict)


def load(path: Path) -> ModuleMap:
    d = tomllib.loads(Path(path).read_text())
    return ModuleMap(
        rails={k: list(v) for k, v in d["rails"].items()},
        rail_role=dict(d["rail_role"]),
        balls={int(k): v for k, v in d["balls"].items()},
        bank_vccio={int(k): float(v) for k, v in d["bank_vccio"].items()},
        pio={int(k): v for k, v in d.get("pio", {}).items()},
        notes=dict(d.get("notes", {})),
    )


def pin_for_ball(m: ModuleMap, ball: str) -> int:
    for pin, b in m.balls.items():
        if b == ball:
            return pin
    raise KeyError(f"ball {ball} is not on the SODIMM")
