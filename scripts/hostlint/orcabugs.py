#!/usr/bin/env python3
"""orcabugs: constructs ORCA/C 2.3.0 b1 is known to miscompile (TOOLCHAIN.md
"Known miscompiles"), found in the VM's C on libclang. Each was measured on
a2min: a minimal program that the optimiser's bit 1 (intermediate-code
peephole) turns into a stack imbalance, so the function returns into data.

  1. An explicit cast of an UNSIGNED 64-bit subtraction inside an arm of ?:,
     e.g. `neg ? (int64_t)((uint64_t)0 - m) : (int64_t)m`, or even
     `up ? m : (uint64_t)(m - 1)`. The same difference uncast, a signed one,
     or a sum is compiled correctly.
  2. A 64-bit ?: with a call in an arm: `return s ? propagate_nan(a, b) : a`
     returns into data (measured); `r = s ? g() : a; return r;` and two calls
     happened to work, but the rule is the shape, not the luck -- write an if.

usage: orcabugs.py FILE.c... -- CLANG_ARGS...    exit 1 on a finding
"""
import sys
import clang.cindex as ci

K = ci.CursorKind
U64 = {"uint64_t", "unsigned long long", "const uint64_t", "unsigned long long int"}
I64 = U64 | {"long long", "int64_t", "unsigned long long"}


def strip(c):
    while c.kind in (K.PAREN_EXPR, K.UNEXPOSED_EXPR):
        ch = list(c.get_children())
        if len(ch) != 1:
            break
        c = ch[0]
    return c


def binop(c):
    """The operator of a BINARY_OPERATOR cursor: the first token after its LHS."""
    ch = list(c.get_children())
    for t in c.get_tokens():
        if t.extent.start.offset >= ch[0].extent.end.offset:
            return t.spelling
    return None


def walk(c):
    yield c
    for ch in c.get_children():
        yield from walk(ch)


def main():
    argv = sys.argv[1:]
    sep = argv.index("--") if "--" in argv else len(argv)
    files, cargs = argv[:sep], argv[sep + 1:]
    found = 0
    for f in files:
        tu = ci.Index.create().parse(f, args=cargs)
        for c in walk(tu.cursor):
            if c.kind != K.CONDITIONAL_OPERATOR or c.location.file is None or c.location.file.name != f:
                continue
            if c.type.get_canonical().spelling in I64 and any(
                    strip(arm).kind == K.CALL_EXPR or (strip(arm).kind == K.CSTYLE_CAST_EXPR and any(
                        strip(g).kind == K.CALL_EXPR for g in strip(arm).get_children()))
                    for arm in list(c.get_children())[1:]):
                print(f"{f}:{c.location.line}: a 64-bit ?: with a call in an arm -- ORCA/C"
                      " miscompiles it (TOOLCHAIN.md); write an if")
                found += 1
            for arm in list(c.get_children())[1:]:
                for x in walk(arm):
                    if x.kind != K.CSTYLE_CAST_EXPR:
                        continue
                    inner = [strip(ch) for ch in x.get_children() if ch.kind != K.TYPE_REF]
                    if inner and inner[-1].kind == K.BINARY_OPERATOR and binop(inner[-1]) == "-" \
                            and inner[-1].type.get_canonical().spelling in U64 | {"unsigned long long"}:
                        print(f"{f}:{x.location.line}: a cast of an unsigned 64-bit difference inside ?:"
                              " -- ORCA/C miscompiles it (TOOLCHAIN.md); compute it in an if")
                        found += 1
    print(f"orcabugs: {found} finding(s)", file=sys.stderr)
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
