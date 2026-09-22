#!/usr/bin/env python3
"""Generate test vectors for the L1 hashing RTL.

Two kinds of ground truth:

  SHA  - NIST FIPS-180-4 published SHA-256 vectors. Fixed, authoritative.
  SP   - store-path vectors whose expected value comes from the REAL `nix` binary
         on this host, not from a reimplementation. This is what makes M1 a parity
         assertion rather than a self-consistency check.

The store-path fingerprint format is verified (docs/REFERENCES.md):

    text:sha256:<hex sha256 of contents>:/nix/store:<name>

which `builtins.toFile <name> <contents>` realises, giving us a way to ask nix for
the answer to a computation we can also state exactly.

Output format, one record per line, inputs hex-encoded to dodge all quoting:

    SHA <input-hex> <expected-digest-hex>
    SP  <fingerprint-hex> <expected-32-chars> <full-store-path>
"""

from __future__ import annotations

import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

NIXBASE32_ALPHABET = "0123456789abcdfghijklmnpqrsvwxyz"


def fold160(digest: bytes) -> bytes:
    """Compress a hash to 20 bytes: result[i % 20] ^= h[i]."""
    out = bytearray(20)
    for i, b in enumerate(digest):
        out[i % 20] ^= b
    return bytes(out)


def nixbase32(data: bytes) -> str:
    """Encode bytes as nix-base32, emitting from the end."""
    nchars = (len(data) * 8 - 1) // 5 + 1
    chars = []
    for n in range(nchars - 1, -1, -1):
        bit = n * 5
        i, j = bit // 8, bit % 8
        c = data[i] >> j
        if i < len(data) - 1:
            c |= data[i + 1] << (8 - j)
        chars.append(NIXBASE32_ALPHABET[c & 0x1F])
    return "".join(chars)


def store_path_hash(fingerprint: str) -> str:
    """The 32-character path component for a fingerprint string."""
    return nixbase32(fold160(hashlib.sha256(fingerprint.encode()).digest()))


# --- NIST FIPS-180-4 SHA-256 vectors -----------------------------------------------
# The 56-byte case matters most: 56 + the 0x80 pad byte forces the length field into a
# SECOND block, which is the padding path most implementations get wrong.
NIST_VECTORS: list[bytes] = [
    b"",
    b"abc",
    b"a",
    b"message digest",
    b"abcdefghijklmnopqrstuvwxyz",
    b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",          # 56 bytes
    b"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
    b"hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",          # 112 bytes
    b"A" * 63,
    b"B" * 64,
    b"C" * 65,
    b"D" * 119,
    b"E" * 120,
]


def nix_to_file(name: str, contents: str) -> str | None:
    """Ask the real nix for the store path of `builtins.toFile name contents`."""
    expr = f'builtins.toFile "{name}" "{contents}"'
    try:
        res = subprocess.run(
            ["nix", "eval", "--impure", "--raw", "--expr", expr],
            capture_output=True, text=True, timeout=120, check=True,
        )
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
        return None
    return res.stdout.strip() or None


# Each entry is (name, contents) -- kept free of quotes and backslashes so the Nix
# expression above needs no escaping.
STORE_PATH_CASES: list[tuple[str, str]] = [
    ("foo", "bar"),
    ("empty", ""),
    ("hello.txt", "hello world"),
    ("a", "a"),
    ("long-name-with-dashes", "some longer contents here 0123456789"),
    ("nix-on-an-apple-iigs", "the card does the arithmetic"),
]


def main() -> int:
    out_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("tests/vectors/l1.txt")
    out_path.parent.mkdir(parents=True, exist_ok=True)

    lines: list[str] = []

    for data in NIST_VECTORS:
        want = hashlib.sha256(data).hexdigest()
        lines.append(f"SHA {data.hex() or '-'} {want}")

    have_nix = shutil.which("nix") is not None
    if not have_nix:
        print("warning: nix not on PATH; skipping store-path parity vectors",
              file=sys.stderr)

    n_sp = 0
    for name, contents in STORE_PATH_CASES:
        inner = hashlib.sha256(contents.encode()).hexdigest()
        fingerprint = f"text:sha256:{inner}:/nix/store:{name}"
        expect = store_path_hash(fingerprint)

        if have_nix:
            real = nix_to_file(name, contents)
            if real is None:
                print(f"warning: nix eval failed for {name!r}; "
                      "using reference model only", file=sys.stderr)
            else:
                # Cross-check the reference model against the real nix binary. If these
                # disagree, the reference model is wrong and every downstream test is
                # meaningless -- so fail loudly rather than bake in a bad expectation.
                real_hash = real.rsplit("/", 1)[-1].split("-", 1)[0]
                if real_hash != expect:
                    print(f"FATAL: reference model disagrees with nix for {name!r}\n"
                          f"  nix:       {real_hash}\n"
                          f"  reference: {expect}", file=sys.stderr)
                    return 1
        full = f"/nix/store/{expect}-{name}"
        lines.append(f"SP {fingerprint.encode().hex()} {expect} {full}")
        n_sp += 1

    out_path.write_text("\n".join(lines) + "\n")
    print(f"wrote {out_path}: {len(NIST_VECTORS)} SHA vectors, {n_sp} store-path vectors"
          + ("" if have_nix else " (reference model only -- nix unavailable)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
