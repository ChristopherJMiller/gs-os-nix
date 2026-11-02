# SPDX-License-Identifier: GPL-3.0-or-later
"""render: the README's 3D pictures of the carrier, with every package and the silkscreen art.

    python3 -m hw.tools.render --board B.kicad_pcb --out DIR [--art ARTWORK.toml]

DIR gets carrier-iso.png (an isometric view), carrier-top.png and carrier-bottom.png. It needs
KiCad's 3D model library, which only the render shell has:

    nix develop .#hw-render

Everything runs on a temporary copy of the board and its project file (kicad-cli rewrites the
.kicad_pro): the silkscreen art (hw/carrier/artwork.toml) is applied to the copy, and J2 gets
our approximate socket body (hw/lib/gsnix.3dshapes, found through ${GSNIX_3DMODEL_DIR}). The
committed board has neither the model reference nor any other 3D edit.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import pcbnew

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from hw.tools import artwork
from hw.tools.footprints import MODEL_NAME

HW = Path(__file__).resolve().parents[1]
SOCKET = "J2"
SHELL_HINT = "run it inside the render shell: nix develop .#hw-render"

# name -> kicad-cli pcb render arguments. The isometric view is the README's hero image.
VIEWS = {
    # Tilted 38 degrees off the top and turned 32, so the socket does not hide the art above it.
    "carrier-iso": ["--rotate", "-38,0,32", "--zoom", "0.75", "--pan", "0,0.1,0", "--perspective"],
    "carrier-top": ["--side", "top"],
    "carrier-bottom": ["--side", "bottom"],
}
SIZE = {"carrier-iso": (2000, 1400), "carrier-top": (1700, 1500), "carrier-bottom": (1700, 1500)}


def attach_socket_model(board: pcbnew.BOARD) -> None:
    """J2's 3D body, by a ${GSNIX_3DMODEL_DIR} path (on the temporary copy only)."""
    fp = board.FindFootprintByReference(SOCKET)
    if fp is None:
        sys.exit(f"render: the board has no {SOCKET}")
    m = pcbnew.FP_3DMODEL()
    m.m_Filename = "${GSNIX_3DMODEL_DIR}/" + MODEL_NAME
    fp.Add3DModel(m)


def run(cmd: list[str], env: dict[str, str]) -> None:
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"render: {' '.join(cmd[:4])} failed ({r.returncode}):\n{r.stdout}{r.stderr}")


def build(board_src: Path, art: Path, out: Path, quality: str = "high") -> list[Path]:
    models = os.environ.get("KICAD10_3DMODEL_DIR")
    if not models or not Path(models).is_dir():
        sys.exit("render: KICAD10_3DMODEL_DIR is unset: the KiCad 3D model library is only in "
                 "the render shell. " + SHELL_HINT)
    env = {**os.environ, "GSNIX_3DMODEL_DIR": os.environ.get("GSNIX_3DMODEL_DIR")
           or str(HW / "lib" / "gsnix.3dshapes")}
    if not (Path(env["GSNIX_3DMODEL_DIR"]) / MODEL_NAME).is_file():
        sys.exit(f"render: no {MODEL_NAME} in {env['GSNIX_3DMODEL_DIR']}")
    out.mkdir(parents=True, exist_ok=True)
    made = []
    with tempfile.TemporaryDirectory() as tmp:
        board = Path(tmp) / board_src.name
        shutil.copy(board_src, board)
        pro = board_src.with_suffix(".kicad_pro")
        if pro.exists():
            shutil.copy(pro, board.with_suffix(".kicad_pro"))
        b = pcbnew.LoadBoard(str(board))
        artwork.apply(b, art)
        attach_socket_model(b)
        b.Save(str(board))
        for name, args in VIEWS.items():
            w, h = SIZE[name]
            dest = out / f"{name}.png"
            run(["kicad-cli", "pcb", "render", "--width", str(w), "--height", str(h),
                 "--quality", quality, "--background", "opaque", *args,
                 "--output", str(dest), str(board)], env)
            made.append(dest)
    return made


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--board", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--art", type=Path, help="default: artwork.toml beside the board")
    ap.add_argument("--quality", default="high", choices=["basic", "high"])
    a = ap.parse_args(argv)
    for p in build(a.board, a.art or a.board.with_name("artwork.toml"), a.out, a.quality):
        print(p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
