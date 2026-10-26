#!/usr/bin/env python3
"""bank-gate: where the VM's code lands, made mechanical (STATUS.md "Bank 0").

Three rules, read from what omf2bin and the linker actually produced (the
GSIM image and the symbol map, `orcalink +S` / `omf2bin --syms`):

  1. Bank 0's size is a ratchet: it must EQUAL the number committed in
     target/vm/bank0.txt. Growth and shrinkage alike are accounted for in the
     commit that causes them (--update rewrites the file), so the diff shows
     every byte of the 40 KB bank that holds the core and every global.
  2. Every ORCA/C runtime helper or libc routine linked into bank 0 must be on
     the allowlist in the same file. A struct copy links ~MOVE, a variable
     left shift ~SHIFTLEFT, a 64-bit divide ~UDIV8: each fails here, named
     with its size and the usual cause.
  3. No symbol in a bank the VM uses for data -- 1 the nursery, 2 the
     collector's stage, 3 the .nbc image, 5 the window ring and remembered set
     (VM.md). omf2bin skips those banks when bank 4 fills; this is the check
     that it did.

usage: bank-gate.py CVM.bin CVM.map BANK0.txt [--update]
       bank-gate.py --self-test CVM.bin CVM.map BANK0.txt
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

DATA_BANKS = {1: "the nursery", 2: "the collector's stage", 3: "the .nbc image",
              5: "the window ring and remembered set"}
HINT = {"~MOVE": "a struct assignment or a struct passed by value",
        "~SHIFTLEFT": "a variable left shift (shift by constants, or BYTE_OF)",
        "~SHIFTRIGHT": "an unsigned variable right shift",
        "~UDIV8": "a 64-bit / or %", "~SDIV8": "a signed 64-bit / or %",
        "~SHL4": "a 32-bit << (BYTE_OF/HI16)", "~LSHR4": "a 32-bit >> (BYTE_OF/HI16)"}
NOT_ROUTINES = {"~GLOBALS", "~ARRAYS"}      # linker-made segments, not code
LIBC = {"memcpy", "memcmp", "memset", "memset_explicit", "memmove", "setjmp", "longjmp",
        "strlen", "strcmp", "strcpy", "malloc", "free", "printf", "sprintf"}


def measure(binp, mapp):
    img = open(binp, "rb").read()
    if img[:4] == b"GSIM":                  # "GSIM", u16 count, then (addr, len) per chunk
        org, b0 = struct.unpack_from("<II", img, 6)
    else:
        org, b0 = 0x2000, len(img)
    syms = sorted({(int(a, 16), n.strip()) for a, n in
                   (l.split(None, 1) for l in open(mapp) if l.strip())})
    lib = {}
    for i, (a, n) in enumerate(syms):
        if a < 0x10000 and ((n.startswith("~") and n not in NOT_ROUTINES) or n in LIBC):
            nxt = next((b for b, _ in syms[i + 1:] if a < b < 0x10000), org + b0)
            lib[n] = nxt - a
    return b0, lib, syms


def read_budget(budp):
    budget, allowed = None, set()
    if os.path.exists(budp):
        for l in open(budp):
            f = l.split()
            if f and f[0] == "bank0":
                budget = int(f[1])
            if f and f[0] == "lib":
                allowed |= set(f[1:])
    return budget, allowed


def check(binp, mapp, budp, quiet=False):
    b0, lib, syms = measure(binp, mapp)
    budget, allowed = read_budget(budp)
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
    if not quiet:
        print(f"bank-gate: {b0} / 40960 bytes in bank 0 (committed: {budget}), "
              f"{len(lib)} runtime routines, {sum(lib.values())} bytes")
        for b in bad:
            print("  FAIL:", b)
    return bad


def update(binp, mapp, budp):
    b0, lib, _ = measure(binp, mapp)
    with open(budp, "w") as f:
        f.write("# Bank 0 of the VM (scripts/bank-gate.py): its size, exactly, and the runtime\n"
                "# routines allowed in it. Rewritten by `just bank-gate --update`.\n")
        f.write(f"bank0 {b0}\nlib {' '.join(sorted(lib))}\n")
    print(f"bank-gate: {budp} now says {b0} bytes, {len(lib)} routines")


def self_test(binp, mapp, budp):
    """D14: every rule must be able to fail. Three plants on copies of the
    real outputs, then one real one: a struct copy compiled by ORCA/C, which
    proves the map is read the way the linker writes it."""
    t = tempfile.mkdtemp()
    ok = True
    try:
        def expect(name, b, m, g, text):
            nonlocal ok
            bad = check(b, m, g, quiet=True)
            hit = any(text in x for x in bad)
            print(f"  {name:<46} {'refused' if hit else 'ACCEPTED -- the rule is decorative'}")
            ok &= hit
        if check(binp, mapp, budp, quiet=True):
            print("  BASELINE FAILS -- fix the tree first"); return False
        g = os.path.join(t, "budget"); m = os.path.join(t, "map")
        b0, lib, _ = measure(binp, mapp)
        open(g, "w").write(f"bank0 {b0 - 1}\nlib {' '.join(sorted(lib))}\n")
        expect("bank 0 one byte larger than committed", binp, mapp, g, "bank 0 is")
        open(g, "w").write(f"bank0 {b0}\nlib {' '.join(sorted(set(lib) - {'memcpy'}))}\n")
        expect("a routine missing from the allowlist", binp, mapp, g, "new routine in bank 0: memcpy")
        with open(m, "w") as f:                 # not shutil.copy: a store file is read-only
            f.write(open(mapp).read() + "050000 planted\n")
        expect("a symbol in bank 5", binp, m, budp, "in bank 5")
        # The real plant: a struct assignment in data.c links ~MOVE.
        src = os.path.join(t, "src")
        shutil.copytree("target/vm/c", src)
        with open(os.path.join(src, "data.c"), "a") as f:
            f.write("\nstruct gate_plant { uint8_t b[9]; };\n"
                    "void gate_plant(struct gate_plant *a, struct gate_plant *b) { *a = *b; }\n")
        srcs = [os.path.join(src, s) for s in
                "main.c run.c data.c prims.c gc.c card.c sha256.c".split()]
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
