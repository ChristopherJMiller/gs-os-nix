# SPDX-License-Identifier: GPL-3.0-or-later
"""fab.py's Interactive HTML BOM step."""
from pathlib import Path

from hw.tools.fab import ibom_command


def test_ibom_waits_for_the_x_server():
    # `xvfb-run -a` starts Xvfb, sleeps a fixed 3 s and runs the command whether or not the
    # server is up. On a loaded build machine Xvfb came up late, wx found no display, and
    # GTK segfaulted (hw-carrier-fab under `nix flake check`, 2026-10-29). Reproduced with
    # an Xvfb shim that starts 6 s late: `-a` failed every time and `-d` passed every time.
    # `-d` (--auto-display) waits on Xvfb's -displayfd, which is written once the server
    # accepts connections.
    cmd = ibom_command(Path("b.kicad_pcb"), Path("out"))
    assert cmd[0] == "xvfb-run"
    assert "-d" in cmd or "--auto-display" in cmd
    assert "-a" not in cmd and "--auto-servernum" not in cmd
