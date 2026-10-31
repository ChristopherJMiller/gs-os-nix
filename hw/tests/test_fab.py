# SPDX-License-Identifier: GPL-3.0-or-later
"""fab.py's Interactive HTML BOM step."""
import os

import pytest
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


# --- 4 layers: the package carries the inner layers and names the stackup -------------------

import csv
import zipfile

import pcbnew

from hw.tools import check_fab, fab


def _package(tmp_path, gerbers, notes=None):
    """A minimal fake package for the carrier board: the files check_fab looks at."""
    board = pcbnew.LoadBoard(str(CARRIER / "carrier.kicad_pcb"))
    d = tmp_path / "pkg"
    (d / "layers").mkdir(parents=True)
    with zipfile.ZipFile(d / "gerbers.zip", "w") as z:
        for n in gerbers:
            z.writestr(n, "x")
    for f in check_fab.FILES:
        if f != "cpl.csv":
            (d / f).write_text("x")
    with (d / "cpl.csv").open("w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["Designator"])
        for r in sorted(check_fab.placed_refs(board)):
            w.writerow([r])
    (d / "order-notes.txt").write_text(fab.order_notes(board) if notes is None else notes)
    return d, board


NAMES = ["x-F_Cu.gtl", "x-inner1.g1", "x-inner2.g2", "x-B_Cu.gbl", "x-F_Mask.gts",
         "x-B_Mask.gbs", "x-F_Silkscreen.gto", "x-B_Silkscreen.gbo", "x-Edge_Cuts.gm1", "x.drl"]


def test_a_complete_four_layer_package_passes(tmp_path):
    d, board = _package(tmp_path, NAMES)
    assert check_fab.problems(d, board) == []


@pytest.mark.parametrize("layer,kikit", [("In1_Cu", "inner1"), ("In2_Cu", "inner2")])
def test_a_package_without_an_inner_gerber_fails(tmp_path, layer, kikit):
    # KiKit names the inner gerbers carrier-inner1.g1 / carrier-inner2.g2 (checked on the board)
    d, board = _package(tmp_path, [n for n in NAMES if kikit not in n])
    assert any(f"no {layer} gerber" in p for p in check_fab.problems(d, board))


def test_the_order_notes_name_the_stackup(tmp_path):
    board = pcbnew.LoadBoard(str(CARRIER / "carrier.kicad_pcb"))
    notes = fab.order_notes(board)
    for want in ("4 layers", "JLC04161H-7628", "1.6 mm", "ENIG", "gold fingers", "45 degree"):
        assert want in notes, want
    d, board = _package(tmp_path, NAMES, notes="two layers, HASL")
    found = check_fab.problems(d, board)
    assert any("order-notes.txt" in p and "JLC04161H-7628" in p for p in found), found
    d, board = _package(tmp_path / "b", NAMES)
    (d / "order-notes.txt").unlink()
    assert any("order-notes.txt is missing" in p for p in check_fab.problems(d, board))
