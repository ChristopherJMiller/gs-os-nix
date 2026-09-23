#!/usr/bin/env python3
"""checks.nbc-opcodes: the opcode table exists in three places and must be one table.

  docs/BYTECODE.md          the normative table       | `$01` | `CONST` | ...
  tools/nbc/src/opcodes.rs  the generator             pub const OP_CONST: u8 = 0x01;
  target/vm/c/opcodes.h     the VM (C, D18)           #define OP_CONST        0x01

A generator and a VM that disagree on one opcode would produce fixtures that run
and return plausible wrong answers -- the failure mode this project guards against
hardest -- so the three are diffed mechanically rather than trusted.
"""
import re
import sys

DOC = "docs/BYTECODE.md"
RS = "tools/nbc/src/opcodes.rs"
H = "target/vm/c/opcodes.h"


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




def from_h(path):
    ops = {}
    decl = re.compile(r"^#define OP_([A-Z_]+)\s+0x([0-9A-Fa-f]{2})\s*$")
    with open(path) as f:
        for line in f:
            m = decl.match(line)
            if m:
                ops[m.group(1)] = int(m.group(2), 16)
    return ops


def prims_from_h(path, names):
    """#define PRIM_LENGTH 0x00 /* 1 */ -- matched back like the .inc form."""
    by_symbol = {n.replace("'", "").upper(): n for n in names}
    prims = {}
    decl = re.compile(r"^#define PRIM_([A-Z0-9_]+)\s+0x([0-9A-Fa-f]{2})\s*/\*\s*(\d+)\s*\*/\s*$")
    with open(path) as f:
        for line in f:
            m = decl.match(line)
            if m and m.group(1) in by_symbol:
                prims[by_symbol[m.group(1)]] = (int(m.group(2), 16), int(m.group(3)))
            elif m:
                prims["PRIM_" + m.group(1)] = (int(m.group(2), 16), int(m.group(3)))
    return prims


def selftest():
    """Prove the checker can fail: copy the three files, change one VM opcode
    value and one arity, and expect a disagreement. A checker that cannot fail
    is decoration."""
    import os
    import shutil
    import subprocess
    import tempfile

    mutations = [
        (H, "#define OP_FORCE        0x41", "#define OP_FORCE        0x43"),
        (H, "#define PRIM_HEAD               0x01 /* 1 */", "#define PRIM_HEAD               0x01 /* 2 */"),
    ]
    for target, before, after in mutations:
        with tempfile.TemporaryDirectory() as tmp:
            for p in (DOC, RS, H):
                os.makedirs(os.path.join(tmp, os.path.dirname(p)), exist_ok=True)
                shutil.copyfile(p, os.path.join(tmp, p))
            path = os.path.join(tmp, target)
            with open(path) as f:
                text = f.read()
            mutated = text.replace(before, after)
            if mutated == text:
                print(f"selftest: PATTERN MISS in {target} -- update the mutation", file=sys.stderr)
                return 1
            with open(path, "w") as f:
                f.write(mutated)
            rc = subprocess.run([sys.executable, os.path.abspath(__file__)], cwd=tmp,
                                capture_output=True).returncode
            if rc == 0:
                print(f"selftest: SURVIVED -- a change in {target} was not caught", file=sys.stderr)
                return 1
    print("NBC OPCODES SELFTEST OK: a changed opcode or arity is caught")
    return 0


def prims_from_doc(path):
    """Rows of the table under '### Primops': | `$00` | `length` | 1 | ..."""
    prims = {}
    row = re.compile(r"^\|\s*`\$([0-9A-Fa-f]{2})`\s*\|\s*`([A-Za-z0-9_']+)`\s*\|\s*(\d+)\s*\|")
    in_section = False
    with open(path) as f:
        for line in f:
            if line.startswith("#"):
                in_section = line.strip() == "### Primops"
                continue
            if not in_section:
                continue
            m = row.match(line)
            if m:
                prims[m.group(2)] = (int(m.group(1), 16), int(m.group(3)))
    return prims


def prims_from_rs(path):
    prims = {}
    row = re.compile(r'^\s*\("([A-Za-z0-9_\']+)", 0x([0-9A-Fa-f]{2}), (\d+)\),')
    with open(path) as f:
        for line in f:
            m = row.match(line)
            if m:
                prims[m.group(1)] = (int(m.group(2), 16), int(m.group(3)))
    return prims




def diff(label, ref, tables):
    bad = 0
    for name, t in tables.items():
        for key in sorted(set(ref) | set(t)):
            a, b = ref.get(key), t.get(key)
            if a != b:
                bad += 1
                print(f"  {label} {key:<18} {DOC}: {a!s:14} {name}: {b!s}")
    return bad


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--selftest":
        return selftest()

    prim_doc = prims_from_doc(DOC)
    prim_tables = {RS: prims_from_rs(RS), H: prims_from_h(H, prim_doc.keys())}
    if not prim_doc or any(not t for t in prim_tables.values()):
        print("primop table: parsed nothing from one source -- a pattern is stale", file=sys.stderr)
        return 1
    prim_bad = diff("primop", prim_doc, prim_tables)

    tables = {DOC: from_doc(DOC), RS: from_rs(RS), H: from_h(H)}
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
    if bad or prim_bad:
        print(f"NBC OPCODES FAIL: {bad + prim_bad} disagreement(s)", file=sys.stderr)
        return 1
    print(f"NBC OPCODES OK: {len(ref)} opcodes and {len(prim_doc)} primops agree across {DOC}, {RS}, {H}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
