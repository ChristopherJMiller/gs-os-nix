# SPDX-License-Identifier: GPL-3.0-or-later
"""fab.py's Interactive HTML BOM step."""
import os
import shutil
import subprocess
from pathlib import Path

from hw.tools.fab import IBOM_ENV, ibom_command

CARRIER = Path(__file__).resolve().parents[1] / "carrier"


def test_ibom_needs_no_x_server():
    # The iBOM ran under xvfb-run, and under a loaded `nix flake check` it failed now and
    # then: wx found no display ("wxEntryStart failed") and GTK segfaulted (2026-10-29, with
    # both `xvfb-run -a` and `-d`). The iBOM has its own headless switch: with
    # INTERACTIVE_HTML_BOM_NO_DISPLAY set it never creates a wx.App, so no X server is
    # involved at all.
    cmd = ibom_command(Path("b.kicad_pcb"), Path("out"))
    assert cmd[0] == "generate_interactive_bom"
    assert "xvfb-run" not in cmd
    assert IBOM_ENV == {"INTERACTIVE_HTML_BOM_NO_DISPLAY": "1"}


def test_ibom_runs_with_no_display(tmp_path):
    shutil.copy(CARRIER / "carrier.kicad_pcb", tmp_path)
    shutil.copy(CARRIER / "carrier.kicad_pro", tmp_path)
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update(IBOM_ENV, HOME=str(tmp_path))
    r = subprocess.run(ibom_command(tmp_path / "carrier.kicad_pcb", tmp_path / "out"),
                       cwd=tmp_path, env=env, capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    page = (tmp_path / "out" / "ibom.html").read_text()
    assert "J2" in page and len(page) > 100_000
