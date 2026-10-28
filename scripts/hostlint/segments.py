#!/usr/bin/env python3
"""segments: every VM source but the first puts its functions in a `segment`
of its own (VM.md "Bank 0", DECISIONS.md D31).

ORCA/C puts a function in the blank segment -- bank 0, which also holds every
global and, above them, the C stack -- unless a `segment "name";` line comes
before it. So a new file without one, or a function written above its file's
`segment` line, lands in bank 0 silently, and scripts/bank-gate.py would see
only bytes. That is how bank 0 filled up the first time. The first file
(main.c) is the exception, and must stay one: its first function is the
load file's entry point, which omf2bin puts in bank 0 (SOURCES).

A function body starts with `{` in column 0, as everywhere in target/vm/c;
nothing else does.

usage: segments.py DIR FILE...   (the FILEs as SOURCES lists them, main.c first)
"""
import re
import sys


def main():
    d, files = sys.argv[1], sys.argv[2:]
    bad = []
    for i, f in enumerate(files):
        lines = open(f"{d}/{f}").read().split("\n")
        seg = next((n for n, l in enumerate(lines) if re.match(r'segment\s+"[^"]+"\s*;', l)), None)
        body = next((n for n, l in enumerate(lines) if l.startswith("{")), None)
        if i == 0 and seg is not None:
            bad.append(f"{f}:{seg + 1}: a `segment` in the first file: its first function is the entry "
                       f"point, which must stay in bank 0")
        elif i > 0 and body is not None and (seg is None or seg > body):
            bad.append(f"{f}:{body + 1}: a function before any `segment` line: it would land in bank 0")
    for b in bad:
        print(b)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
