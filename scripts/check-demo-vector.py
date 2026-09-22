#!/usr/bin/env python3
"""Check that the 65816 demo's hardcoded fingerprint matches the golden vectors.

target/tests/storepath_demo.s embeds a fingerprint string and documents the store
path it should produce. tests/vectors/l1.txt holds the same fingerprint with an
expected value that came from the real `nix` binary.

If those two drift apart, the demo silently starts asserting the wrong answer --
it would still print *a* store path, and it would still look plausible. So this
check exists to make the drift loud.

Exits non-zero on any mismatch.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

DEMO = Path("target/tests/storepath_demo.s")
VECTORS = Path("tests/vectors/l1.txt")


def demo_fingerprint(text: str) -> str:
    """Concatenate the .byte string literals between fingerprint: and its end."""
    body = re.search(
        r"^fingerprint:\s*$(.*?)^fingerprint_end:", text, re.S | re.M
    )
    if not body:
        raise SystemExit(f"{DEMO}: could not find the fingerprint: .. fingerprint_end: block")

    parts: list[str] = []
    for line in body.group(1).splitlines():
        line = line.split(";", 1)[0].strip()
        if not line:
            continue
        if not line.startswith(".byte"):
            raise SystemExit(f"{DEMO}: unexpected directive in fingerprint block: {line!r}")
        parts += re.findall(r'"([^"]*)"', line)
    return "".join(parts)


def demo_expected_path(text: str) -> str | None:
    """The store path the demo's comment claims it will print."""
    m = re.search(r"^;\s+([0-9abcdfghijklmnpqrsvwxyz]{32})\s*$", text, re.M)
    return m.group(1) if m else None


def main() -> int:
    if not DEMO.exists() or not VECTORS.exists():
        print(f"missing {DEMO} or {VECTORS}", file=sys.stderr)
        return 2

    text = DEMO.read_text()
    fp = demo_fingerprint(text)
    claimed = demo_expected_path(text)

    wanted = {}
    for line in VECTORS.read_text().splitlines():
        if not line.startswith("SP "):
            continue
        _, fp_hex, expect, full = line.split()
        wanted[bytes.fromhex(fp_hex).decode()] = (expect, full)

    if fp not in wanted:
        print(f"FAIL: the demo's fingerprint is not in {VECTORS}", file=sys.stderr)
        print(f"  demo: {fp}", file=sys.stderr)
        print("  regenerate with `just vectors`, or fix the demo string.", file=sys.stderr)
        return 1

    expect, full = wanted[fp]

    if claimed is None:
        print(f"FAIL: {DEMO} does not document an expected store path", file=sys.stderr)
        return 1

    if claimed != expect:
        print("FAIL: the demo documents the wrong expected store path", file=sys.stderr)
        print(f"  documented: {claimed}", file=sys.stderr)
        print(f"  vectors:    {expect}", file=sys.stderr)
        return 1

    print(f"ok  demo fingerprint matches vectors ({len(fp)} bytes)")
    print(f"ok  expected output: {expect}")
    print(f"    full path:       {full}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
