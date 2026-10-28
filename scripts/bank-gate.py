#!/usr/bin/env python3
"""bank-gate: where the VM's code and data land, made mechanical (STATUS.md
"Bank 0", VM.md "The C stack", DECISIONS.md D31).

Five rules, read from what omf2bin and the linker actually produced (the
GSIM image and the symbol map, `orcalink +S` / `omf2bin --syms`):

  1. Bank 0's size is a ratchet: it must EQUAL the number committed in
     target/vm/bank0.txt. Growth and shrinkage alike are accounted for in the
     commit that causes them (--update rewrites the file), so the diff shows
     every byte of the bank that holds the core's globals and the C stack.
  2. Every ORCA/C runtime helper or libc routine linked into bank 0 must be on
     the allowlist in the same file. A struct copy links ~MOVE, a variable
     left shift ~SHIFTLEFT, a 64-bit divide ~UDIV8: each fails here, named
     with its size and the usual cause.
  3. Every other symbol in bank 0 -- main.c's functions and every global and
     static -- is in the same file's ledger, with its size. The diff that
     grows bank 0 says which symbol grew, and a function that lands there
     from a file with no `segment` (which c-lint refuses first) is named.
  4. Bank 0 ends at or under vm.h's STACK_FLOOR: above it the C stack runs
     down from STACK_TOP (D31). The two places that repeat those numbers
     agree with vm.h -- the stub omf2bin wrote into the image (its LDA #top)
     and a2min's tripwire (sim/a2min.cpp).
  5. No symbol in a bank the VM uses for data -- 1 the nursery, 2 the
     collector's stage, 3 the .nbc image, 5 the window ring and remembered set,
     7 the regex engine's (VM.md). omf2bin skips 5 and 7 as code fills the
     banks after 4; this is the check that it did.

usage: bank-gate.py CVM.bin CVM.map BANK0.txt [--update]
       bank-gate.py --self-test CVM.bin CVM.map BANK0.txt
From the repo root: rule 4 reads target/vm/c/vm.h and sim/a2min.cpp.
"""
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

VMH, A2MIN = "target/vm/c/vm.h", "sim/a2min.cpp"
DATA_BANKS = {1: "the nursery", 2: "the collector's stage", 3: "the .nbc image",
              5: "the window ring and remembered set", 7: "the regex engine's NFA and stack"}
HINT = {"~MOVE": "a struct assignment or a struct passed by value",
        "~SHIFTLEFT": "a variable left shift (shift by constants, or BYTE_OF)",
        "~SHIFTRIGHT": "an unsigned variable right shift",
        "~UDIV8": "a 64-bit / or %", "~SDIV8": "a signed 64-bit / or %",
        "~SHL4": "a 32-bit << (BYTE_OF/HI16)", "~LSHR4": "a 32-bit >> (BYTE_OF/HI16)"}
NOT_ROUTINES = {"~GLOBALS", "~ARRAYS"}      # linker-made segments, not code
LIBC = {"memcpy", "memcmp", "memset", "memset_explicit", "memmove", "setjmp", "longjmp",
        "strlen", "strcmp", "strcpy", "malloc", "free", "printf", "sprintf"}


def measure(binp, mapp):
    """Bank 0's size and origin, its runtime routines and its other symbols
    (name -> bytes, to the next symbol), and the S the stub starts with."""
    img = open(binp, "rb").read()
    if img[:4] == b"GSIM":                  # "GSIM", u16 count, then (addr, len) per chunk
        org, b0 = struct.unpack_from("<II", img, 6)
        code = img[14:14 + b0]
    else:
        org, b0, code = 0x2000, len(img), img
    # the stub: clc xce rep #$30 tsc sta $03FE, then lda #top tcs (orcarun.cpp omf2bin)
    stub_top = struct.unpack_from("<H", code, 9)[0] if code[8] == 0xA9 and code[11] == 0x1B else None
    syms = sorted({(int(a, 16), n.strip()) for a, n in
                   (l.split(None, 1) for l in open(mapp) if l.strip())})
    lib, ledger = {}, {}
    for i, (a, n) in enumerate(syms):
        if a >= 0x10000 or n in NOT_ROUTINES:
            continue
        size = next((b for b, _ in syms[i + 1:] if a < b < 0x10000), org + b0) - a
        if n.startswith("~") or n in LIBC:
            lib[n] = size
        else:
            ledger[n] = size
    return b0, org, lib, ledger, syms, stub_top


def read_budget(budp):
    budget, allowed, ledger = None, set(), {}
    if os.path.exists(budp):
        for l in open(budp):
            f = l.split()
            if f and f[0] == "bank0":
                budget = int(f[1])
            if f and f[0] == "lib":
                allowed |= set(f[1:])
            if f and f[0] == "sym":
                ledger[f[1]] = int(f[2])
    return budget, allowed, ledger


def memory_map(vmh, a2m):
    """vm.h's STACK_TOP and STACK_FLOOR, and a2min's copy of them."""
    d = {}
    for l in open(vmh):
        m = re.match(r"#define\s+(STACK_TOP|STACK_FLOOR)\s+0x([0-9A-Fa-f]+)U?\b", l)
        if m:
            d[m.group(1)] = int(m.group(2), 16)
    m = re.search(r"constexpr uint16_t VM_STACK_TOP = 0x([0-9A-Fa-f]+), VM_STACK_FLOOR = 0x([0-9A-Fa-f]+);",
                  open(a2m).read())
    return d, ((int(m.group(1), 16), int(m.group(2), 16)) if m else None)


def check(binp, mapp, budp, vmh=VMH, a2m=A2MIN, quiet=False):
    b0, org, lib, ledger, syms, stub_top = measure(binp, mapp)
    budget, allowed, want = read_budget(budp)
    bad = []
    for a, n in syms:
        if (a >> 16) in DATA_BANKS:
            bad.append(f"{n} is at ${a:06X}, in bank {a >> 16}, {DATA_BANKS[a >> 16]}: code over VM data")
    if budget is None:
        bad.append(f"no bank0 line in {budp}; run with --update")
    elif b0 != budget:
        bad.append(f"bank 0 is {b0} bytes, {budp} says {budget} ({b0 - budget:+d}); "
                   f"account for it in the commit and run with --update")
    for n in sorted(set(lib) - allowed):
        bad.append(f"new routine in bank 0: {n}, {lib[n]} bytes -- {HINT.get(n, 'the .map says who calls it')}")
    for n in sorted(set(ledger) - set(want)):
        bad.append(f"new in bank 0: {n}, {ledger[n]} bytes -- a global, or a function from a file "
                   f"with no `segment`; account for it and run with --update")
    for n in sorted(set(want) - set(ledger)):
        bad.append(f"gone from bank 0: {n} ({want[n]} bytes in {budp}); run with --update")
    for n in sorted(set(want) & set(ledger)):
        if want[n] != ledger[n]:
            bad.append(f"{n} is {ledger[n]} bytes in bank 0, {budp} says {want[n]} ({ledger[n] - want[n]:+d})")
    d, a2 = memory_map(vmh, a2m)
    if len(d) != 2:
        bad.append(f"no STACK_TOP and STACK_FLOOR in {vmh}")
    else:
        top, floor = d["STACK_TOP"], d["STACK_FLOOR"]
        if org + b0 > floor:
            bad.append(f"bank 0 ends at ${org + b0:04X}, over the C stack's floor (STACK_FLOOR ${floor:04X}, "
                       f"{vmh}): move code into a `segment`, or lower the floor (D31)")
        if stub_top != top:
            shown = "nowhere it can be read" if stub_top is None else f"${stub_top:04X}"
            bad.append(f"the stub starts S at {shown}; {vmh}'s STACK_TOP is ${top:04X}")
        if a2 is None:
            bad.append(f"no VM_STACK_TOP and VM_STACK_FLOOR in {a2m}")
        elif a2 != (top, floor):
            bad.append(f"a2min's tripwire has top ${a2[0]:04X}, floor ${a2[1]:04X}; "
                       f"{vmh} says ${top:04X}, ${floor:04X}")
    if not quiet:
        room = f", {d['STACK_FLOOR'] - org - b0} under the stack's floor" if len(d) == 2 else ""
        print(f"bank-gate: {b0} bytes in bank 0 (committed: {budget}){room}; "
              f"{len(lib)} runtime routines, {sum(lib.values())} bytes; "
              f"{len(ledger)} other symbols, {sum(ledger.values())} bytes")
        for n, s in sorted(ledger.items(), key=lambda x: -x[1])[:6]:
            print(f"  {s:>6} {n}")
        for b in bad:
            print("  FAIL:", b)
    return bad


def update(binp, mapp, budp):
    b0, _, lib, ledger, _, _ = measure(binp, mapp)
    with open(budp, "w") as f:
        f.write("# Bank 0 of the VM (scripts/bank-gate.py): its size, exactly, the runtime\n"
                "# routines allowed in it, and every other symbol in it with its size.\n"
                "# Rewritten by `just bank-gate --update`.\n")
        f.write(f"bank0 {b0}\nlib {' '.join(sorted(lib))}\n")
        for n in sorted(ledger):
            f.write(f"sym {n} {ledger[n]}\n")
    print(f"bank-gate: {budp} now says {b0} bytes, {len(lib)} routines, {len(ledger)} other symbols")


def self_test(binp, mapp, budp):
    """D14: every rule must be able to fail. Plants on copies of the real
    outputs and sources, then one real one: a struct copy compiled by ORCA/C,
    which proves the map is read the way the linker writes it."""
    t = tempfile.mkdtemp()
    ok = True
    try:
        def expect(name, b, m, g, text, vmh=VMH, a2m=A2MIN):
            nonlocal ok
            bad = check(b, m, g, vmh, a2m, quiet=True)
            hit = any(text in x for x in bad)
            print(f"  {name:<46} {'refused' if hit else 'ACCEPTED -- the rule is decorative'}")
            ok &= hit

        def copy(src, name, old, new):
            p = os.path.join(t, name)
            s = open(src).read()
            assert s.count(old) == 1, f"PATTERN MISS in {src}: {old!r} -- update this self-test"
            open(p, "w").write(s.replace(old, new))     # not shutil.copy: a store file is read-only
            return p
        if check(binp, mapp, budp, quiet=True):
            print("  BASELINE FAILS -- fix the tree first"); return False
        g = os.path.join(t, "budget"); m = os.path.join(t, "map")
        b0, org, lib, ledger, _, _ = measure(binp, mapp)
        syms = "".join(f"sym {n} {s}\n" for n, s in ledger.items())
        open(g, "w").write(f"bank0 {b0 - 1}\nlib {' '.join(sorted(lib))}\n{syms}")
        expect("bank 0 one byte larger than committed", binp, mapp, g, "bank 0 is")
        open(g, "w").write(f"bank0 {b0}\nlib {' '.join(sorted(set(lib) - {'memcpy'}))}\n{syms}")
        expect("a routine missing from the allowlist", binp, mapp, g, "new routine in bank 0: memcpy")
        open(g, "w").write(f"bank0 {b0}\nlib {' '.join(sorted(lib))}\n" +
                           "".join(f"sym {n} {s - (n == 'vstack')}\n" for n, s in ledger.items()))
        expect("a global one byte larger than its entry", binp, mapp, g, "vstack is")
        with open(m, "w") as f:
            f.write(open(mapp).read() + f"{org + 0x101:06X} planted\n")
        expect("a symbol new to bank 0", binp, m, budp, "new in bank 0: planted")
        with open(m, "w") as f:
            f.write(open(mapp).read() + "050000 planted\n")
        expect("a symbol in bank 5", binp, m, budp, "in bank 5")
        with open(m, "w") as f:
            f.write(open(mapp).read() + "070000 planted\n")
        expect("a symbol in bank 7", binp, m, budp, "in bank 7")
        floor = re.search(r"#define STACK_FLOOR +0x[0-9A-F]+U", open(VMH).read()).group(0)
        top = re.search(r"#define STACK_TOP +0x[0-9A-F]+U", open(VMH).read()).group(0)
        low = copy(VMH, "low.h", floor, re.sub(r"0x[0-9A-F]+U", f"0x{org + b0 - 1:04X}U", floor))
        expect("bank 0 over the stack's floor", binp, mapp, budp, "over the C stack's floor", vmh=low)
        moved = copy(VMH, "top.h", top, re.sub(r"0x[0-9A-F]+U", "0xBEFFU", top))
        expect("a stub whose S is not STACK_TOP", binp, mapp, budp, "the stub starts S", vmh=moved)
        a2 = re.search(r"VM_STACK_FLOOR = 0x[0-9A-F]+;", open(A2MIN).read()).group(0)
        a2m = copy(A2MIN, "a2min.cpp", a2, re.sub(r"0x[0-9A-F]+", "0x6F00", a2))
        expect("a2min's tripwire on another floor", binp, mapp, budp, "a2min's tripwire", a2m=a2m)
        # The real plant: a struct assignment in data.c links ~MOVE.
        src = os.path.join(t, "src")
        shutil.copytree("target/vm/c", src)
        with open(os.path.join(src, "data.c"), "a") as f:
            f.write("\nstruct gate_plant { uint8_t b[9]; };\n"
                    "void gate_plant(struct gate_plant *a, struct gate_plant *b) { *a = *b; }\n")
        srcs = [os.path.join(src, s) for s in open("target/vm/c/SOURCES").read().split("\n")
                if s and not s.startswith("#")]
        out = os.path.join(t, "cvm.bin")
        r = subprocess.run(["orcabuild", "-o", out, "-O", *srcs], capture_output=True, text=True)
        if r.returncode != 0:
            print("  the struct-copy plant DID NOT BUILD"); print(r.stdout[-800:], r.stderr[-800:]); return False
        expect("a struct copy (links ~MOVE)", out, os.path.join(t, "cvm.map"), budp, "~MOVE")
    finally:
        shutil.rmtree(t)
    return ok


def main():
    a = sys.argv[1:]
    if a and a[0] == "--self-test":
        print("=== bank gate self-test (D14) ===")
        ok = self_test(*a[1:4])
        print("BANK GATE SELF-TEST OK" if ok else "BANK GATE SELF-TEST FAILED")
        return 0 if ok else 1
    binp, mapp, budp = a[:3]
    if "--update" in a:
        update(binp, mapp, budp)
        return 0
    return 1 if check(binp, mapp, budp) else 0


if __name__ == "__main__":
    sys.exit(main())
