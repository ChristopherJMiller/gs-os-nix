#!/usr/bin/env bash
# Prove the VM's collector tests can FAIL (DECISIONS.md D14, D20).
#
# Every way of getting the remembered set wrong is silent: a field that is not
# recorded, not evacuated or not written back leaves a card object holding a
# stale nursery address, which reads back as some other object -- a plausible
# wrong answer. The same is true of an object too small to hold its forwarding
# pointer. So this breaks the VM in specific ways, one at a time, rebuilds it
# with ORCA/C, and requires the fixtures below to notice in at least one mode:
#
#   limit:0   a normal run
#   limit:1   the remembered set shrunk to one entry, so it fills
#   period:K  GC stress at every K-th boundary, which puts collections between
#             a `let`'s ENV_NEW and its ENV_SETs, and at every phase of a
#             program. Not stress at EVERY boundary: that promotes each value
#             before it can be stored, so the set is never used.
#   reserve:1 every nursery_reserve() collects (A2MIN_RESERVE_STRESS): each is a
#             mid-instruction collection, so a site that keeps a view or a
#             local across one reads a forwarded object
#   mask:M    a normal run with verbs M on the CPU (A2MIN_VERB_MASK): 4 puts
#             SHA-256 on sha256.c, a path no other mode reaches
#   both:K    period:K with a one-entry set. The only mode in which ENV_SET
#             finds the set full: without it, "store_field ignores a full set"
#             survived, because only the period modes reach store_field with a
#             young value and only limit:1 fills the set. K = 4 because it
#             takes two young stores between collections: measured on the
#             mutated VM, K = 2 and 3 miss it and every K from 4 to 9 catches it.
#
# MUTATION_ONLY=<substring> runs just the mutations whose name contains it.
#
# The mutations are applied to a COPY of the sources in a temporary directory,
# never to the tree. The first version edited the tree in place and restored it
# from a trap; a SIGKILL (the low-memory reaper) skips every trap, and twice
# left a mutation behind in target/vm/c.
#
# Usage: mutation-vm.sh <a2min> <nbc>
# Needs orcabuild on PATH (the devShell, or the flake check's inputs).
set -uo pipefail

A2MIN="$1"
NBC="$2"
# The derivation fixtures first: they are cheap, and run_suite stops at the
# first wrong answer, so a mutation they catch costs seconds. The refusal is a
# fixture too -- its .exp is the error line (scripts/scoreboard.py).
FIXTURES="eval-okay-floats eval-okay-float-identity eval-fail-float-add-string eval-fail-float-ceil-range eval-fail-float-compare eval-fail-float-concat eval-fail-float-div-zero eval-fail-float-floor-nan eval-fail-float-floor-precision eval-fail-float-interp eval-okay-tofile eval-fail-tofile-contents eval-fail-tofile-name eval-fail-tofile-name-context eval-fail-tofile-output eval-fail-tofile-whole eval-okay-context-builtins eval-fail-context-adddrv-output eval-fail-context-adddrv-source eval-fail-context-adddrv-two eval-fail-context-append-alloutputs eval-fail-context-append-hash eval-fail-context-append-key eval-fail-context-append-output-ctx eval-fail-context-append-outputs eval-fail-context-getattr eval-fail-context-hasattr eval-fail-context-hascontext-path eval-fail-context-hashstring eval-fail-context-listtoattrs eval-fail-context-removeattrs eval-okay-int-limits eval-fail-int-overflow-add eval-fail-int-overflow-sub eval-fail-int-overflow-neg eval-fail-int-overflow-mul eval-fail-int-overflow-mul-min eval-fail-int-overflow-square eval-fail-int-overflow-div eval-fail-int-overflow-uncaught eval-okay-derivation eval-okay-derivation-fixed-output eval-fail-derivation-fixed-bin eval-fail-derivation-fixed-outputs eval-fail-derivation-hash-base16-digit eval-fail-derivation-hash-base64-digit eval-fail-derivation-hash-empty eval-fail-derivation-hash-length eval-fail-derivation-hash-mode eval-fail-derivation-hash-nix32-digit eval-fail-derivation-hash-nix32 eval-fail-derivation-hash-prefix eval-fail-derivation-hash-sri-long eval-fail-derivation-hash-sri-short eval-fail-derivation-hash-type eval-fail-derivation-hash-untyped refuse-derivation-hash-blake3 refuse-derivation-hash-git refuse-derivation-hash-text eval-okay-derivation-context eval-okay-derivation-escapes eval-okay-derivation-nulls eval-okay-derivation-equality eval-okay-tostring-lists eval-okay-derivation-deep eval-fail-path-plus-context eval-okay-remembered-set eval-okay-deep-equality eval-okay-big-operations eval-okay-paths eval-okay-coerce-sets eval-okay-hashstring"
MODES="limit:0 limit:1 period:2 period:3 both:4 reserve:1 mask:4"
SRCS=$(grep -v '^#' target/vm/c/SOURCES)           # link order: main.c first
WORK=$(mktemp -d)
BUDGET=500000000
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/orig" "$WORK/src"
for f in $SRCS; do cp "target/vm/c/$f" "$WORK/orig/"; done
cp target/vm/c/*.h "$WORK/orig/"                   # vm.h, opcodes.h, prims.h, ...
for t in $FIXTURES; do
    "$NBC" "tests/fixtures/$t.nix" -o "$WORK/$t.nbc" || { echo "cannot compile $t" >&2; exit 1; }
done

# Build the VM from $WORK/src and run every fixture in every mode. 0 = right
# answers everywhere, 1 = something was wrong (or stopped), 2 = did not build.
run_suite() {
    local s list=""
    for s in $SRCS; do list="$list $WORK/src/$s"; done
    orcabuild -o "$WORK/cvm.bin" -O $list > "$WORK/build.log" 2>&1 || return 2
    local t mode got want
    for t in $FIXTURES; do
        want=$(tr -d '\n' < "tests/fixtures/$t.exp")
        for mode in $MODES; do
            case $mode in
                limit:*)  got=$(A2MIN_RSET_LIMIT=${mode#*:} A2MIN_BUDGET=$BUDGET "$A2MIN" \
                                --run-nbc "$WORK/cvm.bin" "$WORK/$t.nbc" 2>&1 | sed -n 's/^result: //p; /^error: /p') ;;
                period:*) got=$(A2MIN_GC_PERIOD=${mode#*:} A2MIN_BUDGET=$BUDGET "$A2MIN" \
                                --run-nbc-gc-stress "$WORK/cvm.bin" "$WORK/$t.nbc" 2>&1 | sed -n 's/^result: //p; /^error: /p') ;;
                both:*)   got=$(A2MIN_GC_PERIOD=${mode#*:} A2MIN_RSET_LIMIT=1 A2MIN_BUDGET=$BUDGET "$A2MIN" \
                                --run-nbc-gc-stress "$WORK/cvm.bin" "$WORK/$t.nbc" 2>&1 | sed -n 's/^result: //p; /^error: /p') ;;
                reserve:*) got=$(A2MIN_RESERVE_STRESS=${mode#*:} A2MIN_BUDGET=$BUDGET "$A2MIN" \
                                --run-nbc "$WORK/cvm.bin" "$WORK/$t.nbc" 2>&1 | sed -n 's/^result: //p; /^error: /p') ;;
                mask:*)   got=$(A2MIN_VERB_MASK=${mode#*:} A2MIN_BUDGET=$BUDGET "$A2MIN" \
                                --run-nbc "$WORK/cvm.bin" "$WORK/$t.nbc" 2>&1 | sed -n 's/^result: //p; /^error: /p') ;;
            esac
            [ "$got" = "$want" ] || return 1
        done
    done
    return 0
}

fresh() { rm -rf "$WORK/src"; cp -r "$WORK/orig" "$WORK/src"; }

pass=0
fail=0

# mutate <name> <file> <sed-expression>
# A pattern that does not match is a FAILURE, not a skip: an inert mutation
# would report "caught" forever while testing nothing. One that does not build
# is a failure too -- it would be caught for the wrong reason.
mutate() {
    local name="$1" file="$2" expr="$3"
    case "$name" in *"${MUTATION_ONLY:-}"*) ;; *) return ;; esac
    fresh
    sed -i "$expr" "$WORK/src/$file"
    if cmp -s "$WORK/orig/$file" "$WORK/src/$file"; then
        printf '  %-52s PATTERN MISS -- update this script\n' "$name"
        fail=$((fail + 1)); return
    fi
    run_suite
    case $? in
        0) printf '  %-52s SURVIVED -- the test is decorative!\n' "$name"; fail=$((fail + 1)) ;;
        2) printf '  %-52s DID NOT BUILD -- fix the mutation\n' "$name"; fail=$((fail + 1)) ;;
        *) printf '  %-52s caught\n' "$name"; pass=$((pass + 1)) ;;
    esac
}

echo "=== mutation testing the collector (remembered set, object sizes, reserves) ==="
echo "Baseline: the fixtures must pass in every mode on the unmutated VM."
fresh
run_suite
case $? in
    0) echo "  baseline passes" ;;
    2) echo "  BASELINE DID NOT BUILD" >&2; cat "$WORK/build.log" >&2; exit 1 ;;
    *) echo "  BASELINE FAILED -- fix the VM before mutation testing." >&2; exit 1 ;;
esac
echo

mutate "a store into a mature field is not recorded" data.c \
       's/    if (IS_YOUNG(v)) remember(c, off);//'

mutate "a mature thunk's update is not recorded" run.c \
       's/        if (IS_YOUNG(y)) remember(th, yoff);//'

mutate "a recorded field is not evacuated" gc.c \
       's/        e->val = IS_YOUNG(p) ? evac(p) : 0;/        e->val = 0;/'

mutate "a recorded field is not written back" gc.c \
       's/        card_write_at(PBITS(e->obj) + e->off, \&t, 4);/        (void)t;/'

mutate "store_field ignores a full set" data.c \
       's/    if (IS_YOUNG(v) \&\& rs_full()) {/    if (0) {/'

mutate "a thunk update ignores a full set" run.c \
       's/        if (IS_YOUNG(y) \&\& rs_full()) {/        if (0) {/'

mutate "an empty list is 4 bytes" data.c \
       's/alloc(n ? 4 + 4 \* n : 8)/alloc(4 + 4 * n)/'

mutate "an empty set is 4 bytes" data.c \
       's/alloc(n ? 4 + 12 \* n : 8)/alloc(4 + 12 * n)/'

# nursery_reserve (vm.h): an operation that allocates in proportion to its
# input collects first if it must. Without the reserve, genList 300 is E_NOMEM
# whenever it starts with less than 13 KB free...
mutate "genList does not reserve" prims.c \
       's/^    nursery_reserve_n(n, 44, 4);                \/\* f is still on the stack: a root \*\///'

# ...and a site that reserves but keeps its view across a collection reads a
# forwarded young operand: the right length, the wrong contents.
mutate "++ keeps its views across a collecting reserve" run.c \
       's/            if (nursery_reserve_n(n + i, 4, 4)) view_two(\&a, \&b);/            nursery_reserve_n(n + i, 4, 4);/'

# String + is interpolate(2) (prims.c), which copies each part through a view
# taken AFTER its reserve; a part read without one is a promoted string read
# as if it were in RAM.
mutate "string + copies parts it did not view after its reserve" strings.c \
       '/^    o = str_alloc(len);$/,/^    STRCTX(o, len) = pop();$/ s/^        s = deref(vstack\[at + i\]);$/        s = vstack[at + i];/'

mutate "map keeps its view across a collecting reserve" prims.c \
       's/    if (nursery_reserve_n(n, 32, 4)) l = view_top_as(T_LIST);/    nursery_reserve_n(n, 32, 4);/'

# Paths (BYTECODE.md "Constants"): a PATH has no context word, so the collector
# must size it itself -- the 8-byte default truncates any path longer than 4
# bytes when it is promoted -- and `+` must canonicalise as Nix does.
mutate "a PATH is sized by the 8-byte default" gc.c \
       's/^    case T_PATH:   return n ? round4(n) + 4 : 8;    \/\* no context word (HEAP.md) \*\///'

# String coercion (prims.c coerce_top): "${drv}" is an outPath coercion, and
# concatStringsSep must copy each element's COERCED text -- its first version
# copied the set itself and printed its header bytes.
mutate "outPath does not coerce" strings.c \
       's/^        } else if (find_attr(vo, str_outpath, \&ix, \&f)) {$/        } else if (0) {/'

mutate "concatStringsSep copies the set, not its coercion" strings.c \
       's/^        if (coerce) e = deref_follow(vstack\[at + i\]);$/        if (0) e = deref_follow(vstack[at + i]);/'

# SHA-256 (prims.c prim_hashstring): three paths, each reached by some mode --
# HASH_STREAM only for a mature string (the period modes), sha256.c only with
# the verb masked (mask:4), the digest readout on every card run.
mutate "HASH_STREAM starts a byte late" strings.c \
       's/        if (!card_sha256_at(PBITS(so) + 4, n, dig))/        if (!card_sha256_at(PBITS(so) + 5, n, dig))/'

mutate "SHA-256 on the CPU pads the wrong length" sha256.c \
       's/    uint32_t bits = (uint32_t)n << 3; /    uint32_t bits = (uint32_t)n << 2; /'

mutate "the digest is read from the wrong slot" vm.h \
       's/^#define card_sha256(s, n, o)    card_hash((s), (n), (o), 0)$/#define card_sha256(s, n, o)    card_hash((s), (n), (o), 3)/'

mutate "path + keeps .. components" data.c \
       's/        if (len == 2 \&\& PCH(start) == '"'"'.'"'"' \&\& PCH(start + 1) == '"'"'.'"'"') {/        if (0) {/'

# Derivations (docs/DERIVATION.md): each rule the host was measured to follow,
# broken. Every one of these gives a well-formed store path -- only the host's
# own answer in the .exp tells it is the wrong one.
mutate "system is escaped like the builder" derivation.c \
       's/^    put_str(k, PAIR_VAL(d, d->sys), 0); /    put_str(k, PAIR_VAL(d, d->sys), 1); /'

mutate "a tab is not escaped" derivation.c \
       's/ || c == .\\t.) {$/) {/'

mutate "an output does not replace an attribute of its name" derivation.c \
       's/^            if (c == 1) ++i;$//'

mutate "__contentAddressed = false goes into the environment" derivation.c \
       's/^    if (IS_LIT(nm, "__contentAddressed")) return K_CA;$//'

mutate "a second output's path has no -<output> suffix" derivation.c \
       's/ol = IS_LIT(ov, "out") ? 0 : AUX16(ov);/ol = 0;/'

mutate "the .drv is hashed from the masked ATerm" derivation.c \
       's/^    d.masked = 0;$//'

mutate "__ignoreNulls keeps nulls" derivation.c \
       's/            if (deref(peek())->tag == T_NULL) { vsp -= 2; continue; }/            if (0) { vsp -= 2; continue; }/'

mutate "toString puts a space after an empty list" strings.c \
       's/        space = i + 1 < n \&\& !(v->tag == T_LIST \&\& AUX16(v) == 0);/        space = i + 1 < n;/'

mutate "a store path is read from P0, the digest" vm.h \
       's/^#define card_storepath(s, n, o) card_hash((s), (n), (o), 3)$/#define card_storepath(s, n, o) card_hash((s), (n), (o), 0)/'

mutate "the CPU does not fold the store-path hash" sha256.c \
       's/^    for (i = 20; i < 32; ++i) f\[i - 20\] ^= dig\[i\];$//'

mutate "two derivations compare as sets" derivation.c \
       's/    if (is_drv(rstack\[base\]) \&\& is_drv(rstack\[base + 1\]) \&\&/    if (0 \&\&/'

# String context (DERIVATION.md "String context"): every operation that builds
# a string from strings carries their contexts, or a dependent derivation loses
# an input and gets a well-formed, wrong path. eval-okay-derivation-context
# reaches its input through each operation; each mutation drops one.
mutate "interpolation drops the parts' contexts" strings.c \
       's/^    push(ctx_join(at, n));                      \/\* a root while the result is reserved \*\/$/    push(0);/'

mutate "a union keeps only the first context" strings.c \
       's/^        if (x) break;                           \/\* a second list: merge \*\/$/        if (x) continue;/'

mutate "concatStringsSep drops the separator's context" strings.c \
       's/    if (!coerce \&\& (hasctx || CTX(deref(rstack\[b\])))) {/    if (!coerce \&\& hasctx) {/'

mutate "substring drops the context" strings.c \
       's/^    STRCTX(r, end - start) = CTX(s);$//'

mutate "baseNameOf drops the context" strings.c \
       's/^    if (s->tag == T_STRING) STRCTX(r, last - start + 1) = CTX(s);   \/\* a string.s context stays \*\/$//'

mutate "dirOf's \".\" and \"/\" drop the context" strings.c \
       's/^    if (!x) { push(st); return; }$/    { push(st); return; }/'

mutate "inputs are hashed by path, not modulo hash" derivation.c \
       's/d->replaced ? KEY_HEX : KEY_DRV/d->replaced ? KEY_DRV : KEY_DRV/'

mutate "an input's modulo hash ignores its own inputs" derivation.c \
       's/^    } else if (hasdrv) {                        \/\* the modulo hash for dependents: \*\/$/    } else if (0) {/'

mutate "the .drv's path names no references" derivation.c \
       's/^    put_refs(\&k, \&d);                           \/\* "text:<every input .drv and source>" \*\/$//'

# A .drv in a string ("=<drv>"): its whole closure, every .drv with all its
# outputs and as a source too (eval-okay-derivation-deep, the host's answers).
mutate "a closure's .drv is not a source" derivation.c \
       's/^        ctx_insert(base, vstack\[cb + i\]);$//'

mutate "a closure's .drv brings only one output" derivation.c \
       's/ctx_insert(base, ctx_piece(0, list_elem(rstack\[rsp - 2\], j, m), rstack\[rsp - 1\], 0, CTX_REST));/ctx_insert(base, ctx_piece(0, list_elem(rstack[rsp - 2], 0, m), rstack[rsp - 1], 0, CTX_REST));/'

mutate "the closure stops at the named .drv" derivation.c \
       's/^            l = ELEMS(deref(drv_entry(p, m)))\[DT_INS\];$/            l = 0;/'

mutate "a derivation records no inputs for a later closure" derivation.c \
       's/^    ELEMS(e)\[DT_INS\] = vstack\[d.ctx\];$/    ELEMS(e)[DT_INS] = 0;/'

mutate "path + a string with context is allowed" run.c \
       's/^            if (b->tag == T_STRING \&\& STRCTX(b, AUX16(b))) type_err();.*$//'

# Fixed outputs (DERIVATION.md "Fixed outputs"): the path, the ATerm's fields
# and the modulo hash, each from the declared hash; then Nix's reading of the
# hash, rule by rule. Most of these give a well-formed path; the eval-fail
# fixtures catch the ones that would accept what Nix rejects.
mutate "a fixed output's path is input-addressed" derivation.c \
       's/^    if (d.fixed != 0xFFFF) {                    \/\* makeFixedOutputPath: from the hash, never the ATerm \*\/$/    if (0) {/'

mutate "a sha256 NAR output is not a source path" derivation.c \
       's/^        if (d.falgo == HA_SHA256 \&\& d.fmeth == FM_NAR) {$/        if (0) {/'

mutate "recursive is flat" derivation.c \
       's/^    if (IS_LIT(s, "nar") || IS_LIT(s, "recursive")) return FM_NAR;$/    if (IS_LIT(s, "nar") || IS_LIT(s, "recursive")) return FM_FLAT;/'

mutate "the r: of a NAR hash is dropped" derivation.c \
       's/^    if (d->fmeth == FM_NAR) put(k, "r:", 2);$//'

mutate "the fixed hash is printed as 32 bytes" derivation.c \
       's/^    put_hex(k, d->fh, ha_size(d->falgo));$/    put_hex(k, d->fh, 32);/'

mutate "the ATerm has no fixed-output fields" derivation.c \
       's/^        if (d->fixed == 0xFFFF) {$/        if (1) {/'

mutate "a fixed output's modulo hash is its ATerm's" derivation.c \
       's/^    if (d.fixed != 0xFFFF) {                    \/\* a fixed output.s modulo hash: the hash and \*\/$/    if (0) {/'

mutate "a fixed output's modulo hash leaves out its path" derivation.c \
       's/^        put(\&k, STRBYTES(d.path\[0\]), AUX16(d.path\[0\]));$//'

mutate "a fixed output may have another output" derivation.c \
       's/^        if (d.no != 1 || !IS_LIT(deref(OUT_NAME(\&d, 0)), "out")) type_err();$/        if (!IS_LIT(deref(OUT_NAME(\&d, 0)), "out")) type_err();/'

mutate "a fixed output's one output may have any name" derivation.c \
       's/^        if (d.no != 1 || !IS_LIT(deref(OUT_NAME(\&d, 0)), "out")) type_err();$/        if (d.no != 1) type_err();/'

mutate "outputHashAlgo is ignored" derivation.c \
       's/^            { s = deref(peek()); d.falgo = hash_algo(STRBYTES(s), AUX16(s)); }$/            { s = deref(peek()); }/'

mutate "an unknown outputHashAlgo is an error" derivation.c \
       's/^            { s = deref(peek()); d.falgo = hash_algo(STRBYTES(s), AUX16(s)); }$/            { s = deref(peek()); if (!(d.falgo = hash_algo(STRBYTES(s), AUX16(s)))) type_err(); }/'

mutate "blake3 is an unknown algorithm" derivation.c \
       's/^    if (IS_BYTES(p, n, "blake3")) err_exit(E_UNSUP);$//'

mutate "text and git are flat" derivation.c \
       's/^    if (IS_LIT(s, "text") || IS_LIT(s, "git")) err_exit(E_UNSUP);$/    if (IS_LIT(s, "text") || IS_LIT(s, "git")) return FM_FLAT;/'

mutate "an empty hash needs no algorithm" derivation.c \
       's/^        if (d->falgo == HA_NONE) type_err();$//'

mutate "the hash and outputHashAlgo need not agree" derivation.c \
       's/^        if (d->falgo != HA_NONE \&\& d->falgo != a) type_err();$//'

mutate "an unknown prefix falls back to outputHashAlgo" derivation.c \
       's/^        if ((a = hash_algo(p, i)) == HA_NONE) type_err();$/        if ((a = hash_algo(p, i)) == HA_NONE) a = d->falgo;/'

mutate "an SRI hash is read by its length" derivation.c \
       's/^    } else if (sri || n == (sz + 2) \/ 3 \* 4) {$/    } else if (n == (sz + 2) \/ 3 * 4) {/'

mutate "base16 is lower case only" derivation.c \
       's/^    if (c >= .A. \&\& c <= .F.) return (uint8_t)(c - .A. + 10);$//'

mutate "base16 takes any character" derivation.c \
       's/^            if ((v = hex_digit(p\[i\])) == 16) type_err();$/            v = hex_digit(p[i]);/'

mutate "nix32 is read from the front" derivation.c \
       's/!= p\[n - 1 - j\]; ++v) ;$/!= p[j]; ++v) ;/'

mutate "nix32 takes any character" derivation.c \
       's/^            if (v == 32) type_err();$//'

mutate "nix32 ignores bits past the hash" derivation.c \
       's/^                    if (i == sz) type_err();$//'

mutate "Base64 does not stop at =" derivation.c \
       's/^        for (i = j = 0, t = 0; i < n \&\& p\[i\] != .=.; ++i) {$/        for (i = j = 0, t = 0; i < n; ++i) {/'

mutate "Base64 takes any character" derivation.c \
       's/^            if ((v = b64_digit(p\[i\])) == 64) type_err();$/            v = b64_digit(p[i]);/'

mutate "Base64 may be short" derivation.c \
       's/^        if (j != sz) type_err();$//'

# builtins.toFile: a text store path from the contents and their references
# (eval-okay-tofile, the host's answers), each rule broken.
mutate "a toFile path has no references" derivation.c \
       '/^void prim_tofile/,/^}/ s/^        put(\&k, ":", 1);$//'

mutate "toFile takes an output in its contents" derivation.c \
       's/^        if (STRBYTES(e)\[0\] != .\/.) type_err();  \/\* "may not reference derivations" \*\/$//'

mutate "a closure does not follow a file's references" derivation.c \
       's/^    ELEMS(e)\[DT_INS\] = CTX(deref(vstack\[b - 1\]));$/    ELEMS(e)[DT_INS] = 0;/'

mutate "a toFile path's context is not itself" derivation.c \
       's/^    STRCTX(v, AUX16(v)) = ctx_one("", 0, STRBYTES(v), AUX16(v));$//'

mutate "toFile takes a name with a context" derivation.c \
       '/^void prim_tofile/,/^}/ s/^    if (CTX(nm)) type_err();                    \/\* forceStringNoCtx \*\/$//'

# The context builtins and the names that may not carry a context
# (eval-okay-context-builtins and the eval-fail-context-* fixtures, the host's
# answers): each rule, broken.
mutate "hasContext is always false" strings.c \
       's/^    push(CTX(s) ? obj_true : obj_false);$/    push(CTX(s) \&\& 0 ? obj_true : obj_false);/'

mutate "unsafeDiscardStringContext keeps the context" strings.c \
       's/^    if (CTX(deref(peek()))) vstack\[vsp - 1\] = str_with_ctx(peek(), 0);$//'

mutate "unsafeDiscardOutputDependency keeps the =" strings.c \
       's/^        if (STRBYTES(deref(e))\[0\] == .=.) ctx_insert(base, ctx_piece(0, 0, e, 1, CTX_REST));$/        if (STRBYTES(deref(e))[0] == 0x3D) ctx_insert(base, ctx_piece(0, 0, e, 0, CTX_REST));/'

mutate "addDrvOutputDependencies takes an output" strings.c \
       's/^    if (p\[0\] == .!. || !is_drv_name(p, m)) type_err();$/    if (!is_drv_name(p, m)) type_err();/'

mutate "addDrvOutputDependencies takes a plain source" strings.c \
       's/^    if (p\[0\] == .!. || !is_drv_name(p, m)) type_err();$/    if (p[0] == 0x21) type_err();/'

mutate "addDrvOutputDependencies takes two elements" strings.c \
       's/^    if (!x || AUX16(deref(x)) != 1) type_err();         \/\* "must have exactly one element" \*\/$/    if (!x) type_err();/'

mutate "getContext drops path = true" strings.c \
       's/^            else if (STRBYTES(ev)\[0\] != .!.) path = 1;$/            else if (STRBYTES(ev)[0] != 0x21) ;/'

mutate "getContext cuts an output's name" strings.c \
       's/^            else push(ctx_piece(0, 0, e, 1, ko - 2));$/            else push(ctx_piece(0, 0, e, 1, ko - 3));/'

mutate "appendContext takes a key that is no store path" strings.c \
       's/^        if (!is_store_path(STRBYTES(nmv), AUX16(nmv))) type_err();   \/\* "is not a store path" \*\/$//'

mutate "a store path's hash may have any letter" strings.c \
       's/^        if (d == 32) return 0;$//'

mutate "appendContext takes allOutputs of a plain path" strings.c \
       's/^                if (!is_drv_name(STRBYTES(nmv), AUX16(nmv))) type_err();$//'

mutate "appendContext takes outputs of a plain path" strings.c \
       's/^            if (m \&\& !is_drv_name(STRBYTES(nmv), AUX16(nmv))) type_err();$//'

mutate "appendContext takes an output name with a context" strings.c \
       '/^void prim_appendcontext/,/^}/ s/^                if (CTX(v)) type_err();         \/\* forceStringNoCtx \*\/$//'

mutate "appendContext drops the string's own context" strings.c \
       's/^    x = CTX(deref(vstack\[b0 - 2\]));             \/\* s.s own context joins them \*\/$/    x = 0;/'

mutate "getAttr and hasAttr take a name with a context" prims.c \
       's/^    if (CTX(v)) type_err();                     \/\* forceStringNoCtx: a name has no context \*\/$//'

mutate "removeAttrs takes a name with a context" prims.c \
       '/^static void prim_removeattrs/,/^}/ s/^        if (CTX(e)) type_err();                 \/\* forceStringNoCtx \*\/$//'

mutate "listToAttrs takes a name with a context" prims.c \
       's/^        if (CTX(nm)) type_err();                \/\* forceStringNoCtx \*\/$//'

mutate "hashString takes a type with a context" strings.c \
       '/^void prim_hashstring/,/^}/ s/^    if (CTX(t)) type_err();                     \/\* forceStringNoCtx \*\/$//'

# Integer overflow (Nix 2.34: an error, never a wrap): each check, broken. The
# limits fixture catches a check that fires too early; the eval-fail fixtures
# one that does not fire.
mutate "a sum's overflow is not checked" prims.c \
       's/^    if (SIGN(x) == SIGN(y) \&\& SIGN(r) != SIGN(x)) err_exit(E_OVERFLOW);$//'

mutate "a difference's overflow is not checked" prims.c \
       's/^    if (SIGN(x) != SIGN(y) \&\& SIGN(r) != SIGN(x)) err_exit(E_OVERFLOW);$//'

mutate "a difference is checked like a sum" prims.c \
       's/^    if (SIGN(x) != SIGN(y) \&\& SIGN(r) != SIGN(x)) err_exit(E_OVERFLOW);$/    if (SIGN(x) == SIGN(y) \&\& SIGN(r) != SIGN(x)) err_exit(E_OVERFLOW);/'

mutate "a negative product may not reach the minimum" prims.c \
       's/^    lim = (a\[7\] ^ b\[7\]) \& 0x80 ? (uint64_t)1 << 63 : ((uint64_t)1 << 63) - 1;$/    lim = ((uint64_t)1 << 63) - 1;/'

mutate "a positive product may reach 2^63" prims.c \
       's/^    lim = (a\[7\] ^ b\[7\]) \& 0x80 ? (uint64_t)1 << 63 : ((uint64_t)1 << 63) - 1;$/    lim = (uint64_t)1 << 63;/'

mutate "a wide product is not checked" prims.c \
       's/^    if (ub > lim \/ ua) err_exit(E_OVERFLOW);$//'

mutate "the fast path takes a 32-bit unsigned operand" prims.c \
       's/^    return x\[3\] \& 0x80 ? (x\[4\] \& x\[5\] \& x\[6\] \& x\[7\]) == 0xFF : (x\[4\] | x\[5\] | x\[6\] | x\[7\]) == 0;$/    return (x[4] | x[5] | x[6] | x[7]) == 0;/'

mutate "the minimum / -1 is not checked" prims.c \
       '/^    if (op != A_MUL) {/,/^    }$/ s/^            err_exit(E_OVERFLOW);$/            ;/'

mutate "unary minus is not checked" run.c \
       's/^        push(new_int(sub_checked(0, INTVAL(a))));   \/\* -x is 0 - x, as in Nix \*\/$/        push(new_int(-INTVAL(a)));/'

mutate "an overflow is a throw tryEval catches" prims.c \
       's/^    if (SIGN(x) == SIGN(y) \&\& SIGN(r) != SIGN(x)) err_exit(E_OVERFLOW);$/    if (SIGN(x) == SIGN(y) \&\& SIGN(r) != SIGN(x)) err_exit(E_THROW);/'

mutate "MUL and DIV are not checked at all" run.c \
       's/^    alu_overflow(op, PAYLOAD(a), PAYLOAD(b));$//'

mutate "a float divided by zero is not an error" numbers.c \
       's/^        if (f_eq(y, 0)) err_exit(E_DIVZERO);$//'

mutate "a float zero divisor is tested by its bits" numbers.c \
       's/^        if (f_eq(y, 0)) err_exit(E_DIVZERO);$/        if (y == 0) err_exit(E_DIVZERO);/'

mutate "unary minus of a float flips its sign" numbers.c \
       's/^    default:     r = f_sub(0, x);$/    default:     r = x ^ 0x8000000000000000ULL;/'

mutate "a NaN equals everything" numbers.c \
       's/^    return 3;$/    return 1;/'

mutate "a top-level float compares by pointer" run.c \
       's/^            if (a->tag == T_FLOAT || b->tag == T_FLOAT) { push_bool(num_cmp(a, b) == 1); break; }$//'

mutate "an INT never equals a FLOAT inside a value" data.c \
       's/^    if (av->tag == T_FLOAT || bv->tag == T_FLOAT) return num_cmp(av, bv) == 1;   \/\* 1 == 1.0 \*\/$//'

mutate "lists of floats are not ordered" data.c \
       's/^    if (a->tag == T_FLOAT || b->tag == T_FLOAT) return num_order(a, b);   \/\* as doubles; 3 unordered \*\/$//'

mutate "INT + FLOAT is integer arithmetic" numbers.c \
       's/^    return a->tag == T_FLOAT || b->tag == T_FLOAT;$/    return a->tag == T_FLOAT;/'

mutate "floor's INT round trip is not checked" numbers.c \
       's/^    if (v->tag == T_INT \&\& r != INTVAL(v)) type_err();$//'

mutate "floor and ceil take any double" numbers.c \
       's/^    if (!f_round_int(num_bits(v), up, \&r)) type_err();$/    (void)f_round_int(num_bits(v), up, \&r);/'

mutate "floor rounds up" numbers.c \
       's/^void prim_floor(void) { round_prim(0); }$/void prim_floor(void) { round_prim(1); }/'

mutate "typeOf a float is int" prims.c \
       's/^    case T_FLOAT:  push(str_float); break;$/    case T_FLOAT:  push(str_int); break;/'

mutate "toString refuses a float" strings.c \
       's/^    case T_FLOAT:  push(float_str(v)); return;     \/\* %f: "4.200000" (docs\/FLOAT.md) \*\/$//'

mutate "toString reads its float after the reserve" numbers.c \
       's/^    uint16_t n = f_format(num_bits(v), buf);$/    uint16_t n = (nursery_reserve(340), f_format(num_bits(v), buf));/'

mutate "the loader does not know K_FLOAT" main.c \
       's/^            case K_FLOAT:                       \/\* the same shape: kind is tag \*\/$//'

mutate "a FLOAT has no size" gc.c \
       's/case T_INT: case T_FLOAT: case T_THUNK:/case T_INT: case T_THUNK:/'

echo
echo "$pass mutations caught, $fail not caught"
[ "$fail" -eq 0 ] || exit 1
echo "MUTATION PASS: every broken collector, derivation and float rule is caught by the fixtures"
