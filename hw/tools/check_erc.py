# SPDX-License-Identifier: GPL-3.0-or-later
"""ERC gate: SKiDL's electrical rules check on the carrier, which must report no error.

    check_erc.py [MODULE] [--report FILE]
    check_erc.py --selftest [MODULE]

MODULE is the dotted name of the module that builds the carrier (default
`hw.carrier.top`); it must define `build()`, which resets SKiDL, builds the carrier and
returns an object with `.nets` (name -> Net). SKiDL's ERC finds pins of conflicting
types on one net (two outputs), floating inputs, and nets nothing drives. `--selftest`
plants the fault the brief names, a second driver on `slot_d_dir`, and requires ERC to
report an error for it.
"""
import argparse
import importlib
import logging
import sys
from pathlib import Path

if __package__ in (None, ""):  # run as a script: put the repo root on sys.path
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from skidl import ERC, SKIDL, TEMPLATE, Part, Pin  # noqa: E402
from skidl.logger import erc_logger, stop_log_file_output  # noqa: E402

stop_log_file_output()  # SKiDL would drop <script>.erc/.log in the cwd (R6)


def run_erc(report: Path | None = None) -> tuple[int, int, str]:
    """Run ERC on the current circuit: (errors, warnings, the report text)."""
    erc_logger.error.reset()
    erc_logger.warning.reset()
    erc_logger.bare_error.reset()
    erc_logger.bare_warning.reset()
    lines: list[str] = []

    class Grab(logging.Handler):
        def emit(self, record: logging.LogRecord) -> None:
            lines.append(f"{record.levelname}: {record.getMessage()}")

    h = Grab()
    erc_logger.addHandler(h)
    try:
        ERC()
    finally:
        erc_logger.removeHandler(h)
    errors = erc_logger.error.count + erc_logger.bare_error.count
    warnings = erc_logger.warning.count + erc_logger.bare_warning.count
    text = "\n".join(lines) + f"\n{errors} errors, {warnings} warnings\n"
    if report:
        report.write_text(text)
    return errors, warnings, text


def _second_driver(top) -> None:
    """A part whose one pin is an output, tied to slot_d_dir (which the FPGA, J2, already
    drives)."""
    p = Part(tool=SKIDL, name="SECOND_DRIVER", ref_prefix="X", dest=TEMPLATE,
             value="second driver")
    p += Pin(num="1", name="OUT", func=Pin.types.OUTPUT)
    p = p()
    top.nets["slot_d_dir"] += p[1]


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("module", nargs="?", default="hw.carrier.top")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--report", type=Path, help="write the ERC report here")
    args = ap.parse_args(argv)
    top = importlib.import_module(args.module)
    if args.selftest:
        top.build()
        base_errors, _, text = run_erc()
        if base_errors:
            print(f"SELFTEST FAIL: the base carrier has {base_errors} ERC errors\n{text}")
            return 1
        planted = top.build()
        _second_driver(planted)
        errors, _, text = run_erc()
        if not errors:
            print(f"SELFTEST FAIL: a second driver on slot_d_dir raised no ERC error\n"
                  f"{text}")
            return 1
        print(f"a second driver on slot_d_dir: {errors} ERC error(s), as it must")
        print("HW ERC SELFTEST OK")
        return 0
    top.build()
    errors, warnings, text = run_erc(args.report)
    print(text, end="")
    if errors:
        return 1
    print("HW ERC OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
