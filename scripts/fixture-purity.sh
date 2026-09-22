#!/usr/bin/env bash
# checks.fixture-purity -- DECISIONS.md D7 made mechanical.
#
# Pre-generated .nbc files are TEST FIXTURES ONLY. Two things would quietly turn
# them into a load-bearing host component, and this script refuses both:
#
#   1. The generator grows hashing or store-path code. Then a fixture could carry
#      a precomputed answer and a parity milestone (M4) would be a tautology.
#      Enforced by grepping the generator's source and manifest, comments
#      stripped, for any crypto/hash/base32 facility.
#
#   2. A .nbc contains a store path or digest that its .nix source does not
#      already contain verbatim. Enforced by extracting every store-path-shaped,
#      nixbase32-digest-shaped and hex-digest-shaped byte run from each .nbc and
#      requiring it to appear in the sibling .nix.
#
#   fixture-purity.sh [dir...]     check tools/nbc and the given fixture dirs
#                                  (default: tests/fixtures)
#   fixture-purity.sh --selftest   plant both violations in temp copies and
#                                  assert each one is caught
#
# A2_GEN_DIR overrides the generator directory (used by the self-test).
set -uo pipefail

GEN_DIR="${A2_GEN_DIR:-tools/nbc}"
# Case-insensitive. `crypto` catches ring/rustcrypto/etc. by name.
CRYPTO_RE='sha1|sha2|sha256|sha512|md5|blake|digest|hmac|openssl|sodium|crypto|nixbase32|base32|/nix/store'
# What a smuggled answer looks like in a fixture.
SMUGGLE_RE='/nix/store/[0-9a-z]{32}|[0-9abcdfghijklmnpqrsvwxyz]{32}|[0-9a-f]{64}'

fail=0

check_generator() {
    local dir="$1" hits
    # Strip // and # comment lines: the generator is allowed to SAY it has no
    # hashing code; it is not allowed to have any.
    hits=$(cat "$dir"/Cargo.toml "$dir"/src/*.rs 2>/dev/null \
        | grep -vE '^\s*(//|#)' \
        | grep -inE "$CRYPTO_RE" || true)
    if [ -n "$hits" ]; then
        echo "  generator $dir references a hashing/crypto/store-path facility:"
        echo "$hits" | sed 's/^/      /'
        fail=1
    else
        echo "  generator $dir: no hashing, crypto or store-path code"
    fi
}

check_fixture() {
    local nbc="$1" src="${1%.nbc}.nix" runs r
    runs=$(grep -aoE "$SMUGGLE_RE" "$nbc" | sort -u || true)
    [ -z "$runs" ] && return
    while IFS= read -r r; do
        if [ -f "$src" ] && grep -qF -- "$r" "$src"; then
            continue        # a literal in the test's own source, fine
        fi
        echo "  $nbc carries '$r' that is not a literal in ${src##*/}"
        fail=1
    done <<< "$runs"
}

check_dirs() {
    local n=0
    for d in "$@"; do
        [ -d "$d" ] || continue
        while IFS= read -r f; do
            check_fixture "$f"
            n=$((n + 1))
        done < <(find "$d" -name '*.nbc' | sort)
    done
    echo "  scanned $n fixture(s) for smuggled store paths and digests"
}

selftest() {
    local tmp
    tmp=$(mktemp -d)
    # Expanded now: a local is out of scope by the time the EXIT trap runs.
    trap "rm -rf '$tmp'" EXIT

    echo "self-test: the checker must catch both violations"

    # (a) a crypto import in the generator, planted in a copy
    mkdir -p "$tmp/gen/src"
    cp "$GEN_DIR"/Cargo.toml "$tmp/gen/"
    cp "$GEN_DIR"/src/*.rs "$tmp/gen/src/"
    echo 'use sha2::Sha256;' >> "$tmp/gen/src/main.rs"
    if A2_GEN_DIR="$tmp/gen" "$0" "$tmp/none" > /dev/null 2>&1; then
        echo "  SURVIVED: a planted crypto import was not caught"; return 1
    fi
    echo "  caught: planted crypto import"

    # (b) a store path in a fixture that its source does not contain
    mkdir -p "$tmp/fx"
    echo '1 + 1' > "$tmp/fx/smuggled.nix"
    printf 'NBC\001/nix/store/fsl59pk3f4c65048v3w7vgazqwg4d75w-x' > "$tmp/fx/smuggled.nbc"
    if "$0" "$tmp/fx" > /dev/null 2>&1; then
        echo "  SURVIVED: a smuggled store path was not caught"; return 1
    fi
    echo "  caught: smuggled store path"

    # (c) a digest shape with no source at all
    rm -f "$tmp/fx"/*
    printf 'NBC\001%064d' 0 > "$tmp/fx/orphan.nbc"
    if "$0" "$tmp/fx" > /dev/null 2>&1; then
        echo "  SURVIVED: a sourceless digest was not caught"; return 1
    fi
    echo "  caught: sourceless hex digest"

    # (d) the same literal present in the source is allowed
    rm -f "$tmp/fx"/*
    echo '"/nix/store/fsl59pk3f4c65048v3w7vgazqwg4d75w-x"' > "$tmp/fx/literal.nix"
    printf 'NBC\001/nix/store/fsl59pk3f4c65048v3w7vgazqwg4d75w-x' > "$tmp/fx/literal.nbc"
    if ! "$0" "$tmp/fx" > /dev/null 2>&1; then
        echo "  FALSE POSITIVE: a literal from the source was rejected"; return 1
    fi
    echo "  allowed: literal that is in the source"

    echo "FIXTURE PURITY SELFTEST OK"
}

if [ "${1:-}" = "--selftest" ]; then
    selftest
    exit $?
fi

echo "=== fixture purity (D7) ==="
check_generator "$GEN_DIR"
if [ $# -gt 0 ]; then check_dirs "$@"; else check_dirs tests/fixtures; fi
if [ "$fail" -ne 0 ]; then
    echo "FIXTURE PURITY FAIL" >&2
    exit 1
fi
echo "FIXTURE PURITY OK"
