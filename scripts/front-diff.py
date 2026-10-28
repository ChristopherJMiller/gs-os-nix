#!/usr/bin/env python3
"""front-diff: the on-target compiler against nbc, stage by stage
(docs/COMPILER.md, D33). Stage 1, the lexer: `nbc --tokens` prints rnix's
token stream for a file, `a2min --lex` the VM's (target/vm/c/lex.c), and
they must be the same -- every token's kind, start and length -- or both
must refuse the file (it is not UTF-8). Stage 2a, the parser: `nbc --cst`
prints rnix's syntax tree, `a2min --cst` the VM's (parse.c), node for node
and token for token, or both refuse the file (rnix reports an error, which
nbc refuses; the VM's E_PARSE). Stage 2b, the lowering: `nbc --ast` prints
nbc's AST as an S-expression, `a2min --ast` the VM's (lower.c). Stage 3,
the compiler: `nbc -o` writes the image, `a2min --compile` the VM's
(compile.c), and they must be the same byte for byte.

Over every .nix in the repository -- the corpus, the fixtures, the
benchmarks, derivation.nix -- and then over a fuzzer's mutations of them:
bytes inserted, deleted and replaced from Nix's punctuation, multi-byte
UTF-8 and Unicode whitespace, and pieces of other files spliced in. The
inputs nobody writes on purpose are where a port goes wrong.

Then it proves it can fail (D14): VMs built from copies of lex.c with a
bug planted must each be caught.

usage: front-diff.py --nbc NBC --a2min A2MIN --vm CVM.BIN [--stage lex|cst|ast|compile] [--fuzz N] [--seed S] [-j J]
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


# Each stage: nbc's flag and how many numbers a line of its output has to
# compare (lex drops the kind's name), and a2min's flag.
STAGES = {"lex": ("--tokens", lambda f: (f[0], f[2], f[3]), "--lex", 3),
          "cst": ("--cst", lambda f: tuple(f), "--cst", 4),
          "ast": ("--ast", None, "--ast", 0),     # the AST: one S-expression line
          "compile": (None, None, "--compile", 0)}   # the image: its bytes


def image(cmd, out):
    """Run cmd, which writes an image to out: its result, and the bytes or None"""
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=600)
        data = open(out, "rb").read() if os.path.exists(out) and p.returncode == 0 else None
        return p, data
    finally:
        if os.path.exists(out):
            os.unlink(out)


def host(stage, nbc, path):
    flag, pick, _, _ = STAGES[stage]
    if stage == "compile":
        fd, out = tempfile.mkstemp(suffix=".nbc")
        os.close(fd)
        p, data = image([nbc, path, "-o", out], out)
        if p.returncode == 2:
            return "refused"
        return [data] if data is not None else f"nbc failed: {p.stderr.decode(errors='replace').strip()[:100]}"
    p = subprocess.run([nbc, flag, path], capture_output=True)
    if p.returncode == 2:
        return "refused"
    if p.returncode != 0:
        return f"nbc failed: {p.stderr.decode(errors='replace').strip()[:100]}"
    if pick is None:
        return [p.stdout.decode(errors="replace").strip()]
    return [tuple(int(x) for x in pick(f)) for f in (l.split() for l in p.stdout.decode().splitlines())]


def target(stage, a2min, vm, path):
    _, _, flag, width = STAGES[stage]
    data = None
    if stage == "compile":
        fd, out = tempfile.mkstemp(suffix=".nbc")
        os.close(fd)
        p, data = image([a2min, flag, vm, path, out], out)
    else:
        p = subprocess.run([a2min, flag, vm, path], capture_output=True, timeout=600)
    out, err = [], None
    for l in p.stdout.decode(errors="replace").splitlines():
        f = l.split()
        if width == 0 and l.startswith("("):
            out.append(l.strip())
        elif width and len(f) == width and all(x.isdigit() for x in f):
            out.append(tuple(int(x) for x in f))
        elif l.startswith("error: "):
            err = l[len("error: "):]
    if err in ("E_UNSUP", "E_PARSE"):
        return "refused"
    if err:
        return f"error {err}"
    if stage == "compile":
        return [data] if data is not None else "no image"
    return out


def compare(args, path):
    """None if the two agree, else what differs; and whether nbc accepted it"""
    h = host(args.stage, args.nbc, path)
    return differs(args, path, h, target(args.stage, args.a2min, args.vm, path)), not isinstance(h, str)


def differs(args, path, h, t):
    if h == t:
        return None
    if isinstance(h, str) or isinstance(t, str):
        return f"{path}: host {h if isinstance(h, str) else 'tokens'}, target {t if isinstance(t, str) else 'tokens'}"
    if args.stage == "compile":
        a, b = h[0], t[0]
        i = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b)))
        return f"{path}: the images differ at byte {i} (host {len(a)} bytes, target {len(b)})"
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


# A generator of Nix, for the parser: the corners mutants seldom reach --
# every operator, pipes, legacy let, the contextual `or`, __curPos,
# interpolated paths, the `{` lookahead's patterns -- with trivia at token
# boundaries.
TRIVIA = ["", " ", "  ", "\n", " # c\n", " /* c */ ", "\t"]
BINOPS = ["+", "-", "*", "/", "++", "//", "==", "!=", "<", "<=", ">", ">=", "&&", "||", "->", "|>", "<|"]
PATTERNS = ["a", "{ }", "{ a, b }", "{ a ? 1, ... }", "{ ... }", "x @ { a }", "{ a }@x", "{a,}", "{a?b,c}",
            "x @ { a } @ y", "{ a } @ x @ y"]
LEAVES = ["1", "0", "01", "1.5", ".5", "1e3", "2.5E-1", "http://x.y/z", "a", "b", "x1", "_y", "a-b", "a'",
          "or", "__curPos", "true", "null", "a or", "./a", "/a/b", "~/c", "a/b", "./a/${x}", "/${y}/z",
          "<nixpkgs>", "<a/b>", "./a${b}c", "[]", "{}", '"s"',
          # floats at Nix's edges: under the midpoint below 2^-1022 (invalid), at
          # and over it (valid), subnormal, underflow and overflow (invalid)
          "2.2250738585072011e-308", "2.22507385850720113605740979670913197593481954635164564e-308",
          "2.2250738585072012e-308", "2.2250738585072014e-308", "4.9e-324", "1e-400", "1e400", "0.0e-400",
          "9223372036854775807", "9223372036854775808", "-9223372036854775808",
          # sets merged by name, rec and plain, with inherits in each
          "{ a = { x = 1; }; a = rec { inherit y; z = x; }; }", "{ a = rec { x = 1; }; a = { inherit y; }; }",
          "{ a.b = 1; a = { c = 2; }; }", "let a = { x = 1; }; a = rec { inherit x; }; in a",
          "rec { a = { inherit b; }; a = rec { inherit c; }; }", "{ a = 1; a = 2; }"]
STRBITS = ["x", " ", "\\\"", "$", "$$", "''$", "'''", "\n  ", "\\${"]

# The compiler refuses a free variable, as nbc does: a generated program for
# it is closed by one of these, binding every name the generator uses --
# lexically, through a `with` (a run-time lookup and its fallback), or with
# `builtins` in scope -- and draws on the names a compiler resolves specially.
CLOSERS = ["let a = 1; b = 2; c = 3; x = 4; y = 5; x1 = 6; _y = 7; a-b = 8; a' = 9; in ", "with { }; ",
           "a: b: c: x: y: x1: _y: a-b: a': ", "with builtins; ", "{ a, b ? 1, c ? a, x, y, x1, _y, a-b, a', ... }@s: ",
           "let a = 1; in with { b = 2; }; let c = a; x = c; y = x; x1 = 6; _y = 7; a-b = 8; a' = 9; in "]
COMPILE_LEAVES = ["builtins.map", "builtins", "map", "(builtins.foo or 1)", "derivation", "builtins.derivation",
                  "builtins.currentSystem", "builtins.langVersion", "builtins.nixPath", "__findFile", "toString",
                  "(x: x)", "(rec { a = b; b = 1; })", "{ ${a} = 1; }", "(let x = x; in x)", "({ a, b ? a }: b)",
                  "builtins.builtins", "__typeOf", "(with builtins; map)", '(builtins.unsafeGetAttrPos "a" { a = 1; })',
                  "(import ./m0.nix)", "(f: f f)", "(x: y: x y)", "(builtins.foldl' (a: b: a) 0 [])"]
compile_leaves = False      # set for the compile stage: the generator draws on COMPILE_LEAVES too


def leaf(rng):
    return rng.choice(LEAVES + COMPILE_LEAVES) if compile_leaves else rng.choice(LEAVES)


def gen(rng, d=0):
    def ws():
        return rng.choice(TRIVIA) if rng.random() < 0.5 else " "

    def attr():
        r = rng.random()
        if r < 0.7:
            return rng.choice(["a", "b", "c", "or", "x-y"])
        if r < 0.85:
            return '"s' + rng.choice(["", "${a}"]) + '"'
        return "${" + gen(rng, d + 1) + "}"

    def path():
        return ".".join(attr() for _ in range(rng.randint(1, 3)))

    def binds():
        return "".join(path() + ws() + "=" + ws() + gen(rng, d + 1) + ";" + ws() for _ in range(rng.randint(0, 3)))

    if d > 6:
        return leaf(rng)
    r = rng.random()
    if r < 0.12:
        op = rng.choice(BINOPS + ["?"])
        rhs = path() if op == "?" else gen(rng, d + 1)
        return gen(rng, d + 1) + ws() + op + ws() + rhs
    if r < 0.2:
        return "(" + gen(rng, d + 1) + ")" + ws() + gen(rng, d + 1)
    if r < 0.27:
        return rng.choice(["-", "!", "- -"]) + ws() + gen(rng, d + 1)
    if r < 0.34:
        inherit = rng.choice(["", "inherit a b;", "inherit (x) a;", 'inherit "q";'])
        return rng.choice(["{", "rec {", "let {"]) + ws() + binds() + inherit + ws() + "}"
    if r < 0.4:
        return "let" + ws() + binds() + "in" + ws() + gen(rng, d + 1)
    if r < 0.48:
        return rng.choice(PATTERNS) + ws() + ":" + ws() + gen(rng, d + 1)
    if r < 0.54:
        return ("if" + ws() + gen(rng, d + 1) + ws() + "then" + ws() + gen(rng, d + 1) + ws() + "else" + ws() +
                gen(rng, d + 1))
    if r < 0.58:
        return rng.choice(["with", "assert"]) + ws() + gen(rng, d + 1) + ";" + ws() + gen(rng, d + 1)
    if r < 0.66:
        return "[" + ws() + " ".join(gen(rng, d + 1) for _ in range(rng.randint(0, 3))) + ws() + "]"
    if r < 0.72:
        default = (ws() + "or" + ws() + gen(rng, d + 1)) if rng.random() < 0.4 else ""
        return gen(rng, d + 1) + "." + path() + default
    if r < 0.78:
        q = rng.choice(['"', "''"])
        body = "".join(rng.choice(STRBITS + ["${" + gen(rng, d + 1) + "}"]) for _ in range(rng.randint(0, 4)))
        return q + body + q
    if r < 0.86:
        return gen(rng, d + 1) + ws() + gen(rng, d + 1)
    return leaf(rng)


def run(args, quiet=False):
    files = sources()
    bad, took = [], 0
    with ThreadPoolExecutor(args.j) as ex:
        for r, ok in ex.map(lambda f: compare(args, f), files):
            took += ok
            if r:
                bad.append(r)
    if not quiet:
        print(f"  {len(files)} files: {len(files) - len(bad)} the same ({took} accepted by nbc)")
    if args.fuzz:
        rng = random.Random(args.seed)
        texts = [open(f, "rb").read() for f in files]
        tmp = tempfile.mkdtemp()
        try:
            mutants = []
            for k in range(args.fuzz):
                p = os.path.join(tmp, f"m{k}.nix")
                with open(p, "wb") as f:
                    if rng.random() < 0.3:
                        closer = rng.choice(CLOSERS) if args.stage == "compile" else ""
                        f.write((closer + gen(rng)).encode())
                    else:
                        f.write(mutate(rng, rng.choice(texts), texts))
                mutants.append(p)
            fbad, took = [], 0
            with ThreadPoolExecutor(args.j) as ex:
                for r, ok in ex.map(lambda f: compare(args, f), mutants):
                    took += ok
                    if r:
                        fbad.append(r)
            if fbad and not quiet:
                keep = os.path.join(tempfile.gettempdir(), "front-diff-failures")
                os.makedirs(keep, exist_ok=True)
                for r in fbad[:20]:
                    shutil.copy(r.split(":")[0], keep)
                print(f"  (the first failing mutants are kept in {keep})")
            if not quiet:
                print(f"  {args.fuzz} mutants (seed {args.seed}): {args.fuzz - len(fbad)} the same "
                      f"({took} accepted by nbc)")
            bad += fbad
        finally:
            shutil.rmtree(tmp)
    return bad


def self_test(args):
    """Each plant is a real way a port of rnix's tokenizer, its parser, nbc's
    lowering or its compiler goes wrong."""
    plants = [
        ("a CALL before a RETURN stays a CALL", "compile.c",
         "    if (c->len && c->last_call == c->len) *code_at", "    if (0) *code_at"),
        ("a name sorts after its extensions", "compile.c",
         "    return c < 0 || (c == 0 && a->len < b->len);", "    return c < 0;"),
        ("the builtins set's names are not sorted", "compile.c",
         '        names = sorted(push_name(names, "builtins", 8, 0));', '        names = push_name(names, "builtins", 8, 0);'),
        ("a number constant is not shared", "compile.c",
         "            if (memcmp(k->a, v, 8) == 0) return i;", "            if (0) return i;"),
        ("a float is lazy without a thunk", "compile.c",
         "    case X_INT: case X_PATH: case X_LAMBDA: strict(e, ch); return;",
         "    case X_INT: case X_FLOAT: case X_PATH: case X_LAMBDA: strict(e, ch); return;"),
        ("a binding that names itself is a pointer copy", "compile.c",
         "r.kind == R_ENV && r.depth == 0 && r.index > idx)", "r.kind == R_ENV && r.depth == 0 && r.index > idx + 1)"),
        ("a set Nix builds without positions has them", "compile.c",
         "        if (p->line == 0) return 0;\n", ""),
        ("an imported file's positions name its importer", "compile.c",
         "        if (outer_file >= 0) CS.cur_file = file_index(file_name(path));", ""),
        ("derivation.nix without Nix's newline before it", "compile.c",
         '(const uint8_t *)("\\n" DERIVATION_NIX), (uint16_t)(DERIVATION_NIX_LEN + 1)',
         '(const uint8_t *)(DERIVATION_NIX), (uint16_t)(DERIVATION_NIX_LEN)'),
        ("a with's miss on `builtins` has no fallback", "compile.c",
         '    if (!g && !IS(name, "builtins")) {', '    if (!g) {'),
        ("an indented string keeps a space too many", "lower.c",
         "if (cur_dropped >= min_indent) SCRATCH[o++] = ch;", "if (cur_dropped > min_indent) SCRATCH[o++] = ch;"),
        ("a merged rec set's inherit stays an inherit", "lower.c",
         "        if (v->tag == X_INHERIT && rec2 && !rec1) {", "        if (0) {"),
        ("a column counts bytes, not characters", "lower.c",
         "        if ((lsrc[i] & 0xC0) != 0x80) ++c;          /* characters, not bytes */", "        ++c;"),
        ("a float under the midpoint is valid", "lower.c",
         "    if (v == 0x0010000000000000ULL && !f_at_least_mid(t, n)) refuse();\n", ""),
        ("an import that cannot be read is refused", "lower.c",
         "    if (!gsos_import(j, n, dst, (uint16_t)(0xFF00U - src_top), &len)) return import_primop(app, arg);",
         "    if (!gsos_import(j, n, dst, (uint16_t)(0xFF00U - src_top), &len)) refuse();"),
        ("a directory import has no default.nix", "gsos.c",
         "    if (k == K_DIR) {                               /* a directory: its default.nix */",
         "    if (0) {"),
        ("- is right-associative", "parse.c",
         "static uint16_t parse_add(void) { return parse_left_assoc(parse_mul, S_ADD); }",
         "static uint16_t parse_add(void) { return parse_right_assoc(parse_mul, S_ADD); }"),
        ("{ a ? 1 } looks like a set, not a pattern", "parse.c",
         "(p1 == TK_COMMA || p1 == TK_QUESTION || p1 == TK_R_BRACE)", "(p1 == TK_COMMA || p1 == TK_R_BRACE)"),
        ("a checkpoint leaves its trivia outside", "parse.c",
         "static uint16_t checkpoint(void)\n{\n    eat_trivia();", "static uint16_t checkpoint(void)\n{\n    tpeek();"),
        ("a b or c is not (a (b or)) c", "parse.c",
         "    } else if (k == TK_OR) {\n        /* \"a b or c\"", "    } else if (0) {\n        /* \"a b or c\""),
        ("x @ { a } @ y is no error", "parse.c",
         "        if (bound) parse_err();                     /* UnexpectedDoubleBind */\n", ""),
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
            if args.plants and f not in args.plants.split(","):
                continue
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
            stage = {"lex.c": "lex", "parse.c": "cst", "compile.c": "compile"}.get(f, "ast")
            global compile_leaves
            compile_leaves = stage == "compile"
            caught = run(argparse.Namespace(**{**vars(args), "vm": vm, "stage": stage}), quiet=True)
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
    ap.add_argument("--stage", choices=sorted(STAGES), default="lex")
    ap.add_argument("--fuzz", type=int, default=0)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--plants", help="the self-test's plants in these files only (lex.c,compile.c)")
    args = ap.parse_args()
    global compile_leaves
    compile_leaves = args.stage == "compile"
    if args.self_test:
        print("=== front-diff self-test (D14): planted lexer, parser, lowering and compiler bugs ===")
        ok = self_test(args)
        print("FRONT-DIFF SELF-TEST OK" if ok else "FRONT-DIFF SELF-TEST FAILED")
        return 0 if ok else 1
    what = {"lex": "1, the lexer, against rnix's", "cst": "2a, the parser, against rnix's",
            "ast": "2b, the lowering, against nbc's", "compile": "3, the compiler, against nbc's"}[args.stage]
    print(f"=== front-diff: stage {what} ===")
    bad = run(args)
    for b in bad[:20]:
        print("  DIFFERS:", b)
    if bad:
        print(f"FRONT-DIFF FAILED: {len(bad)} input(s) differ")
        return 1
    print(f"FRONT-DIFF OK ({args.stage}): every "
          f"{dict(lex='token', cst='node and token', ast='AST', compile='image')[args.stage]} the same")
    return 0


if __name__ == "__main__":
    sys.exit(main())
