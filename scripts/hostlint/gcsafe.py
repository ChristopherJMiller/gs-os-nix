#!/usr/bin/env python3
"""gcsafe: a C local that is live across a call that may collect, or may turn
the window ring over (vm.h's "one rule", HEAP.md "How long a view lives").

For the gs-os-nix VM (target/vm/c), on libclang; run by scripts/c-lint.sh.

  * The invalidating calls are COMPUTED from the call graph, not listed: any
    function that can reach gc_collect may move every young object; any that
    can reach windows_turnover may overwrite every view.  The VM has no
    function pointers, so the graph is exact.
  * Tracked locals: obj_t * (and parameters), and VIEW-derived pointers --
    char * / uint8_t * / obj_t ** whose value comes from STRBYTES, PAYLOAD,
    ELEMS, ANAMES, AVALUES, HASHES or in_key.  A local is a VIEW if any of its
    assignments comes from deref() & co.; a turnover invalidates only views,
    a collection invalidates everything.
  * "Live across" is source order with a little path sense: the next event of
    the local after the call is a read (and the two are not in exclusive
    if/else or switch arms, and no unconditional return lies between), or the
    call is in a loop and the local is read earlier in the body without an
    assignment on the way round.
  * Rule 2: a local last assigned from a stack (vstack/rstack/pop/peek) or a
    field (ELEMS/AVALUES/...) is an ORIGINAL that may be mature; reading its
    fields (STRBYTES(s), s->tag, ...) without deref() is flagged.
  * Suppress a finding with a trailing comment on the READ's line:
        /* gcsafe: mature */   (or any reason after "gcsafe:")

usage: gcsafe.py [--show-sets] FILE.c... -- CLANG_ARGS...
Exit 1 if anything is found.
"""
import re
import sys
import clang.cindex as ci

K = ci.CursorKind
LOOPS = {K.FOR_STMT, K.WHILE_STMT, K.DO_STMT}
NORETURN = {"err_exit", "type_err", "nomem", "badnbc", "throw_exit", "longjmp"}
VIEW_FNS = {"deref", "deref_follow", "deref_pop", "deref_rpop", "force_pop_as", "view_top_as",
            "view_top_text", "in_elem", "drv_hex", "mature_view"}
VIEW_OUT = {"force_pop_view": {0}, "force_two": {0, 1}, "view_two": {0, 1}, "force_two_orig": {0, 1}}
INTERIOR = {"STRBYTES", "PAYLOAD", "ELEMS", "ANAMES", "AVALUES", "HASHES", "in_key"}
OBJ = {"obj_t *", "struct obj *"}
# rule 2: an ORIGINAL (from a stack or a field) may be mature -- a card address --
# and must go through deref() before its fields are read
ORIG_SRC = {"vstack", "rstack", "pop", "rpop", "peek", "ELEMS", "AVALUES", "ANAMES", "IND_TARGET", "CL_ENV"}
ACCESS = r"(STRBYTES|PAYLOAD|ELEMS|ANAMES|AVALUES|HASHES|AUX16|INTVAL|BOOLVAL|CL_CODE|CL_CTAB|CL_ENV|" \
         r"IND_TARGET|STRCTX|CTX|SET_HDR|need)\s*\(\s*"     # a macro argument's cursor sits on the macro
PTRS = {"char *", "uint8_t *", "unsigned char *", "obj_t **", "struct obj **"}


def walk(c, parents=()):
    yield c, parents
    for ch in c.get_children():
        yield from walk(ch, parents + (c,))


def callee(call):
    r = call.referenced
    return r.spelling if r is not None else None


def toks(c):
    return [t.spelling for t in c.get_tokens()]


def strip(par, i):
    while i >= 0 and par[i].kind in (K.PAREN_EXPR, K.UNEXPOSED_EXPR, K.CSTYLE_CAST_EXPR):
        i -= 1
    return i


def closure(graph, roots):
    s = set(roots)
    while True:
        more = {f for f, cs in graph.items() if f not in s and cs & s}
        if not more:
            return s
        s |= more


def exclusive(a, b, apar, bpar):
    """True if offsets a and b are in different arms of one if/else or switch."""
    for p in apar:
        if p not in bpar:
            continue
        if p.kind == K.IF_STMT:
            ch = list(p.get_children())
            if len(ch) == 3:
                arm = lambda o: 1 if ch[1].extent.start.offset <= o <= ch[1].extent.end.offset else \
                                (2 if ch[2].extent.start.offset <= o <= ch[2].extent.end.offset else 0)
                if {arm(a), arm(b)} == {1, 2}:
                    return True
        if p.kind == K.SWITCH_STMT:
            body = list(p.get_children())[-1]
            labels = sorted(c.extent.start.offset for c, _ in walk(body)
                            if c.kind in (K.CASE_STMT, K.DEFAULT_STMT))
            arm = lambda o: max([l for l in labels if l <= o], default=-1)
            if arm(a) != arm(b) and arm(a) >= 0:
                # exclusive unless a's arm can fall into the next one
                nxt = min([l for l in labels if l > a], default=body.extent.end.offset)
                tail = [t.spelling for t in p.translation_unit.get_tokens(
                    extent=ci.SourceRange.from_locations(
                        ci.SourceLocation.from_offset(p.translation_unit, p.location.file, a),
                        ci.SourceLocation.from_offset(p.translation_unit, p.location.file, nxt)))]
                if {"break", "return", "continue"} & set(tail) or NORETURN & set(tail):
                    return True
    return False


def returns_between(call, cpar, use_off):
    """An unconditional return (or noreturn call) after the call, in the block
    that holds it or any enclosing block short of a loop, before the use and
    with no break/continue/goto on the way."""
    at = call.extent.end.offset
    for blk in reversed(cpar):
        if blk.kind in LOOPS:
            return False
        if blk.kind != K.COMPOUND_STMT:
            continue
        for st in blk.get_children():
            if st.extent.end.offset < at:
                continue
            if st.extent.start.offset <= at <= st.extent.end.offset:
                continue                              # the statement holding the call
            if st.extent.start.offset >= use_off:
                break
            t = toks(st)
            if {"break", "continue", "goto"} & set(t):
                return False
            if st.kind == K.RETURN_STMT or (t and t[0] in NORETURN):
                return True
    return False


def analyse(tus):
    graph, bodies = {}, {}
    for tu in tus:
        for f in tu.cursor.get_children():
            if f.kind == K.FUNCTION_DECL and f.is_definition():
                bodies[f.spelling] = (tu, f)
                graph[f.spelling] = {callee(c) for c, _ in walk(f) if c.kind == K.CALL_EXPR}
    collect = closure(graph, {"gc_collect"})
    turn = closure(graph, {"windows_turnover"}) | collect
    findings = []
    for name, (tu, f) in sorted(bodies.items()):
        body = next((c for c in f.get_children() if c.kind == K.COMPOUND_STMT), None)
        if body is None:
            continue
        decls, view = {}, set()
        for c, _ in walk(f):
            if c.kind in (K.VAR_DECL, K.PARM_DECL):
                ts = c.type.spelling.replace("const ", "")
                if ts in OBJ or (c.kind == K.VAR_DECL and ts in PTRS):
                    decls[c.hash] = (c.spelling, ts)
        if not decls:
            continue
        ev = []                                       # (offset, kind, name, parents, cursor)
        for c in f.get_children():
            if c.kind == K.PARM_DECL and c.hash in decls:
                ev.append((body.extent.start.offset, "def", c.spelling, (), c, False))
        ptr_from = {}                                 # name -> set of tokens on the right of its defs
        origdef = set()                               # offsets of defs that take an original
        for c, par in walk(body):
            if c.kind == K.VAR_DECL and c.hash in decls:
                init = [ch for ch in c.get_children() if ch.kind != K.TYPE_REF]
                if init:
                    rhs = toks(init[-1])
                    if rhs and rhs[0] in ORIG_SRC:
                        origdef.add(c.extent.end.offset)
                    ev.append((c.extent.end.offset, "def", c.spelling, par, c, bool(rhs) and rhs[0] in VIEW_FNS))
                    ptr_from.setdefault(c.spelling, set()).update(rhs)
                    if rhs and rhs[0] in VIEW_FNS:
                        view.add(c.spelling)
            elif c.kind == K.CALL_EXPR and callee(c) in turn:
                ev.append((c.extent.end.offset, "call", callee(c), par, c, False))
            elif c.kind == K.DECL_REF_EXPR and c.referenced is not None and c.referenced.hash in decls:
                v = c.referenced.spelling
                i = strip(par, len(par) - 1)
                p = par[i] if i >= 0 else None
                if p is not None and p.kind == K.BINARY_OPERATOR and \
                        list(p.get_children())[0].extent.start.offset == c.extent.start.offset and \
                        _op(p) == "=":
                    rhs_c = list(p.get_children())[1]
                    rhs = toks(rhs_c)
                    ptr_from.setdefault(v, set()).update(rhs)
                    if rhs and rhs[0] in VIEW_FNS:
                        view.add(v)
                    if rhs and rhs[0] in ORIG_SRC:
                        origdef.add(p.extent.end.offset)
                    ev.append((p.extent.end.offset, "def", v, par, c, bool(rhs) and rhs[0] in VIEW_FNS))
                elif p is not None and p.kind == K.UNARY_OPERATOR and toks(p)[:1] == ["&"]:
                    j = strip(par, i - 1)
                    call = par[j] if j >= 0 and par[j].kind == K.CALL_EXPR else None
                    if call is not None:
                        args = list(call.get_arguments())
                        pos = next((n for n, a in enumerate(args)
                                    if a.extent.start.offset <= c.extent.start.offset <= a.extent.end.offset), -1)
                        isv = pos in VIEW_OUT.get(callee(call), ())
                        if isv:
                            view.add(v)
                        ev.append((call.extent.end.offset + 1, "def", v, par, c, isv))
                    else:
                        ev.append((c.extent.start.offset, "use", v, par, c, False))
                else:
                    ev.append((c.extent.start.offset, "use", v, par, c, False))
        # which locals matter: obj_t * always; other pointers only if view-derived
        live = {}
        for nm, ts in decls.values():
            if ts in OBJ:
                live[nm] = nm in view
            elif INTERIOR & ptr_from.get(nm, set()):
                live[nm] = True                       # an interior pointer: a view in all but name
        ev.sort(key=lambda e: (e[0], {"use": 0, "call": 1, "def": 2}[e[1]]))
        src = open(f.location.file.name).read()
        line = lambda off: src.count("\n", 0, off) + 1
        lines = src.split("\n")
        last = {}
        for (off, kind, v, par, cur, _) in ev:        # rule 2: an original read as if in RAM
            if kind == "def":
                last[v] = off in origdef
            elif kind == "use" and last.get(v) and v in live:
                post = src[off + len(v):off + len(v) + 3]
                if re.match(ACCESS + re.escape(v) + r"\b", src[off:]) or \
                        (src[off:off + len(v)] == v and post.startswith("->")):
                    ln = line(off)
                    if "gcsafe:" not in lines[ln - 1]:
                        findings.append((f.location.file.name, ln, name, v, "original", "-", 0,
                                         "read without deref()"))
        for k, (pc, kind, who, cpar, ccur, _) in enumerate(ev):
            if kind != "call":
                continue
            for v, is_view in live.items():
                before = [e for e in ev[:k] if e[2] == v and e[1] != "call"]
                if who not in collect:                # a turnover leaves originals alone
                    defs = [e for e in before if e[1] == "def"]
                    if not defs or not defs[-1][5]:
                        continue
                after = [e for e in ev[k + 1:] if e[2] == v and e[1] != "call"]
                loops = [p for p in cpar if p.kind in LOOPS]
                hit = None
                if before and after and after[0][1] == "use":
                    u = after[0]
                    ok = exclusive(pc, u[0], cpar, u[3]) or returns_between(ccur, cpar, u[0])
                    for L in loops:                   # out of a for(;;) only via a break
                        if u[0] > L.extent.end.offset and _forever(L):
                            brk = [t for t in L.get_tokens() if t.spelling == "break"]
                            first_brk = min((t.extent.start.offset for t in brk), default=1 << 30)
                            if not [t for t in brk if t.extent.start.offset > pc] and \
                                    any(e[1] == "def" and L.extent.start.offset <= e[0] < first_brk
                                        for e in ev if e[2] == v):
                                ok = True
                    if not ok:
                        hit = (u[0], "read after the call")
                if hit is None:
                    for L in loops:                   # round the back edge
                        lo, hi = L.extent.start.offset, L.extent.end.offset
                        inside = [e for e in ev if e[2] == v and e[1] != "call" and lo <= e[0] <= hi]
                        first = next((e for e in inside if e[0] < pc), None)
                        if first is not None and first[1] == "use" and \
                                not any(e[1] == "def" for e in inside if e[0] > pc):
                            hit = (first[0], "read next time round the loop")
                            break
                if hit:
                    ln = line(hit[0])
                    if "gcsafe:" in lines[ln - 1]:
                        continue
                    findings.append((f.location.file.name, ln, name, v,
                                     "view" if is_view else "local", who, line(pc), hit[1]))
    return collect, turn, findings


def _forever(L):
    t = toks(L)
    return L.kind == K.FOR_STMT and t[:4] == ["for", "(", ";", ";"]


def _op(binop):
    lhs = list(binop.get_children())[0]
    for t in binop.get_tokens():
        if t.extent.start.offset >= lhs.extent.end.offset:
            return t.spelling
    return None


def main():
    argv = sys.argv[1:]
    sep = argv.index("--") if "--" in argv else len(argv)
    files, cargs = argv[:sep], argv[sep + 1:]
    show = "--show-sets" in files
    files = [f for f in files if not f.startswith("--")]
    idx = ci.Index.create()
    tus = [idx.parse(f, args=cargs) for f in files]
    bad = False
    for tu in tus:
        for d in tu.diagnostics:
            if d.severity >= ci.Diagnostic.Error:
                print("parse error:", d, file=sys.stderr)
                bad = True
    collect, turn, findings = analyse(tus)
    if show:
        print("may collect (%d): %s" % (len(collect), " ".join(sorted(collect))))
        print("may turn the ring over only (%d): %s" % (len(turn - collect), " ".join(sorted(turn - collect))))
    seen = set()
    for fn, ln, name, v, kind, who, cl, how in sorted(findings):
        if (fn, ln, v) in seen:
            continue
        seen.add((fn, ln, v))
        if who == "-":
            print(f"{fn}:{ln}: in {name}: {kind} '{v}' {how}: it may be mature")
        else:
            print(f"{fn}:{ln}: in {name}: {kind} '{v}' {how} to {who}() at line {cl}")
    print(f"gcsafe: {len(seen)} finding(s)", file=sys.stderr)
    return 1 if seen or bad else 0


if __name__ == "__main__":
    sys.exit(main())
