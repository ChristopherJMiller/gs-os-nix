#!/usr/bin/env python3
"""repl-session: M3 (docs/COMPILER.md "Stage 4"), a session of typed lines.

tests/repl/NAME.txt is what is typed: a line ends at a CR (the IIgs's
Return key) or an LF, as repl.c reads it. tests/repl/NAME.exp holds an
answer for each line that is not empty, in order: the host's (`nix-instantiate
--eval --strict -E`), or -- for a line Nix rejects too -- the VM's error line,
as an eval-fail fixture's is (D7). `a2min --repl` must write exactly the
transcript they make: a prompt before each line read, and each answer on a
line of its own. Two sessions: `session`, whose heavy lines fill mature
space across lines -- a later line's collection takes an earlier line's
garbage -- and `light`, the same less those, for GC stress, under which the
heavy lines are beyond the VM (E_NOMEM, alone as in a session).

  repl-session.py --a2min A2MIN --vm CVM.BIN [--session NAME] [--gc-stress]
  repl-session.py --update --a2min A2MIN --vm CVM.BIN [--nix nix-instantiate]
      the answers again: the host's, and for a line it rejects the VM's error
      line (printed, to be read)
  repl-session.py --self-test --a2min A2MIN
      VMs built with a REPL bug planted must each fail the session (D14)
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

PROMPT = b"nix> "
SESSION = ANSWERS = None


def use(name):
    global SESSION, ANSWERS
    SESSION, ANSWERS = f"tests/repl/{name}.txt", f"tests/repl/{name}.exp"


def lines_of(data):
    """repl.c's reading: the lines, empty ones included, and whether the
    input ended with a terminator (then a last prompt reads its end)"""
    out, cur, ended = [], bytearray(), True
    for b in data:
        if b in (0x0D, 0x0A):
            out.append(bytes(cur))
            cur = bytearray()
            ended = True
        else:
            cur.append(b)
            ended = False
    if cur:
        out.append(bytes(cur))
    return out, ended


def transcript(data, answers):
    lines, ended = lines_of(data)
    t, k = bytearray(), 0
    for ln in lines:
        t += PROMPT
        if ln:
            t += answers[k] + b"\n"
            k += 1
    if ended:
        t += PROMPT
    return bytes(t)


def run_vm(a2min, vm, gc_stress):
    p = subprocess.run([a2min, "--repl-gc-stress" if gc_stress else "--repl", vm, SESSION],
                       capture_output=True, timeout=3600)
    return p.stdout, p.stderr.decode(errors="replace")


def answers():
    with open(ANSWERS, "rb") as f:
        return [l for l in f.read().split(b"\n") if l]


def check(args, vm=None, quiet=False):
    data = open(SESSION, "rb").read()
    want = transcript(data, answers())
    got, err = run_vm(args.a2min, vm or args.vm, args.gc_stress)
    if got == want:
        if not quiet:
            n = sum(1 for l in lines_of(data)[0] if l)
            print(f"REPL SESSION OK ({args.session}{', GC stress' if args.gc_stress else ''}): {n} lines typed, "
                  f"every answer as the host's"
                  f"; {err.strip()}")
        return True
    if not quiet:
        g, w = got.split(b"\n"), want.split(b"\n")
        for i, (a, b) in enumerate(zip(g, w)):
            if a != b:
                print(f"  line {i + 1}: the VM wrote {a[:200]!r}\n           the session wants {b[:200]!r}")
                break
        else:
            print(f"  the VM wrote {len(g)} lines, the session wants {len(w)}")
        print("REPL SESSION FAILED")
    return False


def update(args):
    data = open(SESSION, "rb").read()
    typed = [l for l in lines_of(data)[0] if l]
    got, _ = run_vm(args.a2min, args.vm, False)
    vm = [l[len(PROMPT):] if l.startswith(PROMPT) else l for l in got.split(b"\n")]
    vm = [l for l in (l.replace(PROMPT, b"") for l in vm) if l]
    out, ok = [], True
    for i, ln in enumerate(typed):
        p = subprocess.run([args.nix, "--eval", "--strict", "-E", ln.decode()], capture_output=True)
        if p.returncode == 0:
            out.append(p.stdout.strip())
        else:
            mine = vm[i] if i < len(vm) else b""
            if not mine.startswith(b"error: "):
                print(f"  {ln!r}: the host rejects it, the VM answers {mine!r}")
                ok = False
            print(f"  {ln.decode()[:60]:<60} host rejects it; the VM's {mine.decode()}")
            out.append(mine)
    with open(ANSWERS, "wb") as f:
        f.write(b"\n".join(out) + b"\n")
    print(f"{ANSWERS}: {len(out)} answers" + ("" if ok else " -- SOME NEED READING"))
    return 0 if ok else 1


def self_test(args):
    """Each plant is a way M3's loop goes wrong that its own lines would hide"""
    plants = [
        ("a CR does not end a line", "if (c == '\\r' || c == '\\n') break;", "if (c == '\\n') break;"),
        ("an empty line is compiled", "        if (n == 0) continue;", "        if (0) continue;"),
        ("the prompt is not written", "        rw(0x2013, out, (void *)PROMPT", "        if (0) rw(0x2013, out, (void *)PROMPT"),
        ("a line is read after the last one", "        n = 0;\n        for (;;) {", "        for (;;) {"),
    ]
    ok = True
    work = tempfile.mkdtemp()
    try:
        for name, old, new in plants:
            src = os.path.join(work, "src")
            shutil.rmtree(src, ignore_errors=True)
            shutil.copytree("target/vm/c", src)
            p = os.path.join(src, "repl.c")
            s = open(p).read()
            if s.count(old) != 1:
                print(f"  {name:<40} PATTERN MISS -- update this self-test"); ok = False; continue
            open(p, "w").write(s.replace(old, new))
            srcs = [os.path.join(src, l.strip()) for l in open("target/vm/c/SOURCES") if l.strip() and not l.startswith("#")]
            vm = os.path.join(work, "cvm.bin")
            r = subprocess.run(["orcabuild", "-o", vm, "-O", *srcs], capture_output=True, text=True)
            if r.returncode != 0:
                print(f"  {name:<40} DID NOT BUILD"); ok = False; continue
            caught = not check(argparse.Namespace(**{**vars(args), "gc_stress": False}), vm=vm, quiet=True)
            print(f"  {name:<40} {'caught' if caught else 'SURVIVED -- the session is decorative'}")
            ok &= caught
    finally:
        shutil.rmtree(work)
    print("REPL SESSION SELF-TEST OK" if ok else "REPL SESSION SELF-TEST FAILED")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a2min", required=True)
    ap.add_argument("--vm")
    ap.add_argument("--gc-stress", action="store_true")
    ap.add_argument("--session", default="session")
    ap.add_argument("--update", action="store_true")
    ap.add_argument("--nix", default="nix-instantiate")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    use(args.session)
    if args.self_test:
        return 0 if self_test(args) else 1
    if args.update:
        return update(args)
    return 0 if check(args) else 1


if __name__ == "__main__":
    sys.exit(main())
