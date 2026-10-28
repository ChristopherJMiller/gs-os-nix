#!/usr/bin/env python3
"""front-diff: the on-target compiler against nbc, stage by stage
(docs/COMPILER.md, D33). Stage 1, the lexer: `nbc --tokens` prints rnix's
token stream for a file, `a2min --lex` the VM's (target/vm/c/lex.c), and
they must be the same -- every token's kind, start and length -- or both
must refuse the file (it is not UTF-8).

Over every .nix in the repository -- the corpus, the fixtures, the
benchmarks, derivation.nix -- and then over a fuzzer's mutations of them:
bytes inserted, deleted and replaced from Nix's punctuation, multi-byte
UTF-8 and Unicode whitespace, and pieces of other files spliced in. The
inputs nobody writes on purpose are where a port goes wrong.

Then it proves it can fail (D14): VMs built from copies of lex.c with a
bug planted must each be caught.

usage: front-diff.py --nbc NBC --a2min A2MIN --vm CVM.BIN [--fuzz N] [--seed S] [-j J]
       front-diff.py --self-test --nbc NBC --a2min A2MIN [--fuzz N]
"""
import argparse
import os
import random
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor


def sources():
    out = []
    for root in ("tests", "tools/nbc/src"):
        for d, _, fs in os.walk(root):
            out += [os.path.join(d, f) for f in fs if f.endswith(".nix")]
    return sorted(out)


def host(nbc, path):
    p = subprocess.run([nbc, "--tokens", path], capture_output=True, text=True)
    if p.returncode == 2:
        return "refused"
    if p.returncode != 0:
        return f"nbc failed: {p.stderr.strip()[:100]}"
    return [tuple(int(x) for x in (f[0], f[2], f[3])) for f in (l.split() for l in p.stdout.splitlines())]


def target(a2min, vm, path):
    p = subprocess.run([a2min, "--lex", vm, path], capture_output=True, text=True, timeout=600)
    toks, err = [], None
    for l in p.stdout.splitlines():
        f = l.split()
        if len(f) == 3 and all(x.isdigit() for x in f):
            toks.append(tuple(int(x) for x in f))
        elif l.startswith("error: "):
            err = l[len("error: "):]
    if err == "E_UNSUP":
        return "refused"
    if err:
        return f"error {err}"
    return toks


def compare(args, path):
    h, t = host(args.nbc, path), target(args.a2min, args.vm, path)
    if h == t:
        return None
    if isinstance(h, str) or isinstance(t, str):
        return f"{path}: host {h if isinstance(h, str) else 'tokens'}, target {t if isinstance(t, str) else 'tokens'}"
    for i, (a, b) in enumerate(zip(h, t)):
        if a != b:
            return f"{path}: token {i}: host {a}, target {b}"
    return f"{path}: host {len(h)} tokens, target {len(t)}"


# Mutations: what a lexer decides on, and what it must step over whole.
PUNCT = list(b"\"'${}()[];:,.=?@!<>/\\*+-~#&|_\n\t ") + [ord(c) for c in "019aeEzZ"]
UNI = ["é", " ", "\u0085", " ", " ", " ", "​", " ", " ",
       " ", "　", "\U0001F600", "Â", "''", "''$", "''\\", "${", "\\\"", "//", "/*", "*/",
       "...", "./", "~/", "<a>", "a:b", "http://x", "1e", "1.5e+3", ".5", "0x", "or", "__curPos"]


def mutate(rng, text, others):
    b = bytearray(text.encode() if isinstance(text, str) else text)
    for _ in range(rng.randint(1, 4)):
        i = rng.randrange(len(b) + 1)
        r = rng.random()
        if r < 0.3:
            b[i:i] = bytes([rng.choice(PUNCT)])
        elif r < 0.55:
            b[i:i] = rng.choice(UNI).encode()
        elif r < 0.7 and b:
            del b[i:i + rng.randint(1, 3)]
        elif r < 0.8 and b:
            b[min(i, len(b) - 1)] = rng.choice(PUNCT)
        elif r < 0.95:
            o = rng.choice(others)
            j = rng.randrange(len(o) + 1)
            b[i:i] = o[j:j + rng.randint(1, 40)]
        else:
            b[i:i] = bytes([rng.choice([0x80, 0xC0, 0xC3, 0xED, 0xF5, 0xFF])])     # not UTF-8
    return bytes(b)


def run(args, quiet=False):
    files = sources()
    bad = []
    with ThreadPoolExecutor(args.j) as ex:
        for r in ex.map(lambda f: compare(args, f), files):
            if r:
                bad.append(r)
    if not quiet:
        print(f"  {len(files)} files: {len(files) - len(bad)} the same")
    if args.fuzz:
        rng = random.Random(args.seed)
        texts = [open(f, "rb").read() for f in files]
        tmp = tempfile.mkdtemp()
        try:
            mutants = []
            for k in range(args.fuzz):
                p = os.path.join(tmp, f"m{k}.nix")
                with open(p, "wb") as f:
                    f.write(mutate(rng, rng.choice(texts), texts))
                mutants.append(p)
            fbad = []
            with ThreadPoolExecutor(args.j) as ex:
                for r in ex.map(lambda f: compare(args, f), mutants):
                    if r:
                        fbad.append(r)
            if fbad and not quiet:
                keep = os.path.join(tempfile.gettempdir(), "front-diff-failures")
                os.makedirs(keep, exist_ok=True)
                for r in fbad[:20]:
                    shutil.copy(r.split(":")[0], keep)
                print(f"  (the first failing mutants are kept in {keep})")
            if not quiet:
                print(f"  {args.fuzz} mutants (seed {args.seed}): {args.fuzz - len(fbad)} the same")
            bad += fbad
        finally:
            shutil.rmtree(tmp)
    return bad


def self_test(args):
    """Each plant is a real way a port of rnix's tokenizer goes wrong."""
    plants = [
        ("NBSP is not whitespace", "lex.c", "(b1 == 0x85 || b1 == 0xA0) ? 2 : 0", "(b1 == 0x85) ? 2 : 0"),
        ("a path may contain //", "lex.c",
         "            if (src[i] == '/' && src[i + 1] == '/') return TK_ERROR;   /* contains \"//\" */", ""),
        ("an unknown character is one byte", "lex.c",
         "    if (c >= 0) off += ulen((uint8_t)c);", "    if (c >= 0) off += 1;"),
        ("'' ends an indented string before ''$", "lex.c",
         "                if (c == '\\'' || c == '$') {", "                if (c == '\\'') {"),
    ]
    ok = True
    work = tempfile.mkdtemp()
    try:
        for name, f, old, new in plants:
            src = os.path.join(work, "src")
            shutil.rmtree(src, ignore_errors=True)
            shutil.copytree("target/vm/c", src)
            p = os.path.join(src, f)
            s = open(p).read()
            if s.count(old) != 1:
                print(f"  {name:<44} PATTERN MISS -- update this self-test"); ok = False; continue
            open(p, "w").write(s.replace(old, new))
            srcs = [os.path.join(src, l.strip()) for l in open("target/vm/c/SOURCES")
                    if l.strip() and not l.startswith("#")]
            vm = os.path.join(work, "cvm.bin")
            r = subprocess.run(["orcabuild", "-o", vm, "-O", *srcs], capture_output=True, text=True)
            if r.returncode != 0:
                print(f"  {name:<44} DID NOT BUILD"); print(r.stdout[-600:], r.stderr[-600:]); ok = False; continue
            caught = run(argparse.Namespace(**{**vars(args), "vm": vm}), quiet=True)
            print(f"  {name:<44} {'caught' if caught else 'SURVIVED -- the diff is decorative'}")
            ok &= bool(caught)
    finally:
        shutil.rmtree(work)
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nbc", required=True)
    ap.add_argument("--a2min", required=True)
    ap.add_argument("--vm")
    ap.add_argument("--fuzz", type=int, default=0)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        print("=== front-diff self-test (D14): planted lexer bugs ===")
        ok = self_test(args)
        print("FRONT-DIFF SELF-TEST OK" if ok else "FRONT-DIFF SELF-TEST FAILED")
        return 0 if ok else 1
    print("=== front-diff: stage 1, the lexer, against rnix's ===")
    bad = run(args)
    for b in bad[:20]:
        print("  DIFFERS:", b)
    if bad:
        print(f"FRONT-DIFF FAILED: {len(bad)} input(s) differ")
        return 1
    print("FRONT-DIFF OK: every token the same")
    return 0


if __name__ == "__main__":
    sys.exit(main())
