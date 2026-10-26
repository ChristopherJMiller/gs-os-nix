#!/usr/bin/env python3
"""Rewrite ORCA/C-only syntax so a host compiler can parse the VM sources.

  segment "x";      ->  /* segment "x"; */
  asm { ... }       ->  orca_asm(&v1, &v2, ...);   every C identifier the block
                         names as an operand, so the host sees it read/written
                         (no false "uninitialised" / "unused" warnings)

Line numbers are preserved (each asm line becomes an empty line) so warnings
point at the real source.  usage: orca2host.py IN OUT
"""
import re, sys

MNEMONIC = re.compile(r"^[a-z]{3}$")
REGS = {"a"}   # x/y/s only appear as ",x" suffixes, stripped above

def convert(text):
    out, lines, i = [], text.split("\n"), 0
    while i < len(lines):
        ln = lines[i]
        if re.match(r'^\s*segment\s+"[^"]*"\s*;', ln):
            out.append("/* " + ln.strip() + " */")
            i += 1
            continue
        m = re.match(r"^(\s*)asm\s*\{\s*$", ln)
        if not m:
            out.append(ln); i += 1; continue
        indent, body, j = m.group(1), [], i + 1
        while not re.match(r"^\s*\}\s*$", lines[j]):
            body.append(lines[j]); j += 1
        labels, ops = set(), []
        for b in body:
            b = b.split(";")[0]
            lm = re.match(r"^\s*([A-Za-z_]\w*):\s*(.*)$", b)
            if lm:
                labels.add(lm.group(1)); b = lm.group(2)
            toks = b.split(None, 1)
            if len(toks) == 2 and MNEMONIC.match(toks[0]):
                for ident in re.findall(r"[A-Za-z_]\w*", re.sub(r",\s*[xys]\b", "", re.sub(r"0x[0-9A-Fa-f]+", "", toks[1]))):
                    ops.append(ident)
        uses = []
        for o in ops:
            if o not in labels and o not in REGS and o not in uses:
                uses.append(o)
        out.append(indent + "orca_asm(0" + "".join(", &" + u for u in uses) + ");")
        out.extend([""] * (j - i))           # body lines and the closing brace
        i = j + 1
    return "\n".join(out)

src, dst = sys.argv[1], sys.argv[2]
with open(src) as f:
    t = f.read()
with open(dst, "w") as f:
    f.write(convert(t))
