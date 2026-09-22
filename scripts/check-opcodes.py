#!/usr/bin/env python3
"""checks.nbc-opcodes: the opcode table exists in three places and must be one table.

  docs/BYTECODE.md          the normative table       | `$01` | `CONST` | ...
  tools/nbc/src/opcodes.rs  the generator             pub const OP_CONST: u8 = 0x01;
  target/vm/opcodes.inc     the 65816 VM              OP_CONST        = $01

A generator and a VM that disagree on one opcode would produce fixtures that run
and return plausible wrong answers -- the failure mode this project guards against
hardest -- so the three are diffed mechanically rather than trusted.
"""
import re
import sys

DOC = "docs/BYTECODE.md"
RS = "tools/nbc/src/opcodes.rs"
INC = "target/vm/opcodes.inc"


def from_doc(path):
    """Rows of the table under '## Opcodes' only -- the constants table uses the
    same `$XX` | `NAME` shape for heap tags, which are a different namespace."""
    ops = {}
    row = re.compile(r"^\|\s*`\$([0-9A-Fa-f]{2})`\s*\|\s*`([A-Z_]+)`\s*\|")
    in_section = False
    with open(path) as f:
        for line in f:
            if line.startswith("## "):
                in_section = line.strip() == "## Opcodes"
                continue
            if not in_section:
                continue
            m = row.match(line)
            if m:
                ops[m.group(2)] = int(m.group(1), 16)
    return ops


def from_rs(path):
    ops = {}
    decl = re.compile(r"^pub const OP_([A-Z_]+): u8 = 0x([0-9A-Fa-f]{2});")
    with open(path) as f:
        for line in f:
            m = decl.match(line)
            if m:
                ops[m.group(1)] = int(m.group(2), 16)
    return ops


def from_inc(path):
    ops = {}
    decl = re.compile(r"^OP_([A-Z_]+)\s*=\s*\$([0-9A-Fa-f]{2})\s*$")
    with open(path) as f:
        for line in f:
            m = decl.match(line)
            if m:
                ops[m.group(1)] = int(m.group(2), 16)
    return ops


def selftest():
    """Prove the checker can fail: copy the three files, change one VM opcode
    value, and expect a disagreement. A checker that cannot fail is decoration."""
    import os
    import shutil
    import subprocess
    import tempfile

    with tempfile.TemporaryDirectory() as tmp:
        for p in (DOC, RS, INC):
            os.makedirs(os.path.join(tmp, os.path.dirname(p)), exist_ok=True)
            shutil.copyfile(p, os.path.join(tmp, p))
        inc = os.path.join(tmp, INC)
        with open(inc) as f:
            text = f.read()
        mutated = text.replace("OP_FORCE        = $41", "OP_FORCE        = $43")
        if mutated == text:
            print("selftest: PATTERN MISS -- update the mutation", file=sys.stderr)
            return 1
        with open(inc, "w") as f:
            f.write(mutated)
        rc = subprocess.run([sys.executable, os.path.abspath(__file__)], cwd=tmp,
                            capture_output=True).returncode
        if rc == 0:
            print("selftest: SURVIVED -- a changed VM opcode was not caught", file=sys.stderr)
            return 1
    print("NBC OPCODES SELFTEST OK: a changed VM opcode is caught")
    return 0


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--selftest":
        return selftest()
    tables = {DOC: from_doc(DOC), RS: from_rs(RS), INC: from_inc(INC)}
    for name, t in tables.items():
        if not t:
            print(f"{name}: parsed no opcodes -- the pattern is stale", file=sys.stderr)
            return 1
        values = list(t.values())
        if len(values) != len(set(values)):
            print(f"{name}: duplicate opcode value", file=sys.stderr)
            return 1

    ref = tables[DOC]
    bad = 0
    for name, t in tables.items():
        if name == DOC:
            continue
        for op in sorted(set(ref) | set(t)):
            a, b = ref.get(op), t.get(op)
            if a != b:
                bad += 1
                fa = f"${a:02X}" if a is not None else "missing"
                fb = f"${b:02X}" if b is not None else "missing"
                print(f"  {op:<14} {DOC}: {fa:8} {name}: {fb}")
    if bad:
        print(f"NBC OPCODES FAIL: {bad} disagreement(s)", file=sys.stderr)
        return 1
    print(f"NBC OPCODES OK: {len(ref)} opcodes agree across {DOC}, {RS}, {INC}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
