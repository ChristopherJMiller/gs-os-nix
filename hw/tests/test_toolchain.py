# SPDX-License-Identifier: GPL-3.0-or-later
"""The .#hw toolchain is what docs/HARDWARE.md says it is: KiCad 10, SKiDL with
the KICAD10 tool, the KiCad symbol libraries, and Freerouting."""
import os
import shutil
import subprocess
from pathlib import Path


def test_pcbnew_is_kicad_10():
    import pcbnew
    assert pcbnew.Version().startswith("10.")


def test_kicad_cli_is_10():
    out = subprocess.run(["kicad-cli", "version"], capture_output=True, text=True).stdout
    assert out.startswith("10.")


def test_symbol_libraries_reachable():
    d = Path(os.environ["KICAD_SYMBOL_DIR"])
    for lib in ["74xx", "74xGxx", "Logic_LevelTranslator", "Device", "Connector_Generic"]:
        assert (d / f"{lib}.kicad_sym").is_file(), lib


def test_skidl_writes_a_netlist(tmp_path):
    from skidl import Part, Net, generate_netlist, reset, set_default_tool, KICAD10
    reset()
    set_default_tool(KICAD10)
    r1, r2 = Part("Device", "R", value="1k"), Part("Device", "R", value="2k")
    n = Net("N_TEST")
    n += r1[1], r2[1]
    generate_netlist(file_=str(tmp_path / "t.net"))
    text = (tmp_path / "t.net").read_text()
    assert "N_TEST" in text and "1k" in text


def test_freerouting_on_path():
    assert shutil.which("freerouting")
