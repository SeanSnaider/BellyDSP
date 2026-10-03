#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""Ed25519 (EdDSA over Curve25519) in pure Python, for the release scripts.

Sparkle (macOS) and WinSparkle (Windows) check every update with an Ed25519 signature: the private
key signs the downloaded file, and the public key compiled into the app verifies it. Sparkle's own
tools (generate_keys, sign_update) only run on macOS, so this file does the same job anywhere Python
does (the Windows CI runner, a test on any machine) with no packages to install:

    ed25519.py keygen <private-key-file>           new random key: writes the private seed, prints the public key
    ed25519.py public <private-key-file>           prints the public key for an existing private key
    ed25519.py sign <private-key-file|-> <file>    prints the base64 signature of a file ('-' reads the key from stdin)
    ed25519.py verify <public-key> <file> <sig>    exits 0 if the signature is valid

Key formats are Sparkle's: the private key file is base64 of the 32-byte seed (what
`generate_keys -x` exports for keys made by Sparkle 2), the public key is base64 of its 32 bytes, and
a signature is base64 of 64 bytes.

The math is RFC 8032 section 5.1 (Josefsson and Liusvaara, "Edwards-Curve Digital Signature
Algorithm", 2017), following its reference implementation in section 6:
  - the curve is the twisted Edwards curve -x^2 + y^2 = 1 + d x^2 y^2 over GF(p), p = 2^255 - 19,
    d = -121665/121666, with base point B of prime order L = 2^252 + 27742317777372353535851937790883648493;
  - points use extended coordinates (X, Y, Z, T), x = X/Z, y = Y/Z, xy = T/Z, so addition needs no inversion;
  - the secret scalar s comes from SHA-512 of the seed (lower half, "clamped": low 3 bits cleared so s is a
    multiple of the cofactor 8, bit 254 set so the ladder has a fixed length), the public key is A = sB;
  - signing: r = SHA-512(prefix || M) mod L, R = rB, k = SHA-512(R || A || M) mod L, S = (r + k s) mod L,
    signature = R || S;
  - verifying: accept if S B == R + k A (checked as [S]B - [k]A == R after encoding).
It isn't constant time, which doesn't matter here: it runs on the release machine, not on a server
anyone else can time.
"""

import base64
import hashlib
import os
import sys

P = 2**255 - 19
L = 2**252 + 27742317777372353535851937790883648493
D = (-121665 * pow(121666, P - 2, P)) % P
SQRT_M1 = pow(2, (P - 1) // 4, P)  # a square root of -1 mod p, used to recover x from y


def _sha512_int(*parts):
    return int.from_bytes(hashlib.sha512(b"".join(parts)).digest(), "little")


def _add(a, b):
    # RFC 8032 5.1.4, addition in extended coordinates (a = -1 twisted Edwards, "add-2008-hwcd-3").
    A = (a[1] - a[0]) * (b[1] - b[0]) % P
    B = (a[1] + a[0]) * (b[1] + b[0]) % P
    C = 2 * a[3] * b[3] * D % P
    Dd = 2 * a[2] * b[2] % P
    E, F, G, H = B - A, Dd - C, Dd + C, B + A
    return (E * F % P, G * H % P, F * G % P, E * H % P)


def _mul(s, point):
    # Double and add, from the lowest bit.
    q = (0, 1, 1, 0)  # the neutral element (0, 1)
    while s > 0:
        if s & 1:
            q = _add(q, point)
        point = _add(point, point)
        s >>= 1
    return q


def _equal(a, b):
    # x1/z1 == x2/z2 and y1/z1 == y2/z2, cross-multiplied.
    return (a[0] * b[2] - b[0] * a[2]) % P == 0 and (a[1] * b[2] - b[1] * a[2]) % P == 0


def _recover_x(y, sign):
    # RFC 8032 5.1.3: x^2 = (y^2 - 1) / (d y^2 + 1); a square root via the exponent (p + 3) / 8.
    if y >= P:
        return None
    x2 = (y * y - 1) * pow(D * y * y + 1, P - 2, P)
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (P + 3) // 8, P)
    if (x * x - x2) % P != 0:
        x = x * SQRT_M1 % P
    if (x * x - x2) % P != 0:
        return None
    if (x & 1) != sign:
        x = P - x
    return x


_GY = 4 * pow(5, P - 2, P) % P
_GX = _recover_x(_GY, 0)
BASE = (_GX, _GY, 1, _GX * _GY % P)


def _compress(point):
    zinv = pow(point[2], P - 2, P)
    x, y = point[0] * zinv % P, point[1] * zinv % P
    return int.to_bytes(y | ((x & 1) << 255), 32, "little")


def _decompress(data):
    if len(data) != 32:
        return None
    y = int.from_bytes(data, "little")
    sign = y >> 255
    y &= (1 << 255) - 1
    x = _recover_x(y, sign)
    if x is None:
        return None
    return (x, y, 1, x * y % P)


def _expand(seed):
    if len(seed) != 32:
        raise ValueError("an Ed25519 private key seed is 32 bytes")
    h = hashlib.sha512(seed).digest()
    s = int.from_bytes(h[:32], "little")
    s &= (1 << 254) - 8   # clear the low 3 bits and bit 255
    s |= 1 << 254         # set bit 254
    return s, h[32:]


def public_key(seed):
    s, _ = _expand(seed)
    return _compress(_mul(s, BASE))


def sign(seed, message):
    s, prefix = _expand(seed)
    a = _compress(_mul(s, BASE))
    r = _sha512_int(prefix, message) % L
    rs = _compress(_mul(r, BASE))
    k = _sha512_int(rs, a, message) % L
    return rs + int.to_bytes((r + k * s) % L, 32, "little")


def verify(public, message, signature):
    if len(public) != 32 or len(signature) != 64:
        return False
    a = _decompress(public)
    r = _decompress(signature[:32])
    if a is None or r is None:
        return False
    s = int.from_bytes(signature[32:], "little")
    if s >= L:
        return False
    k = _sha512_int(signature[:32], public, message) % L
    return _equal(_mul(s, BASE), _add(r, _mul(k, a)))


def _read_seed(path):
    text = sys.stdin.read() if path == "-" else open(path).read()
    raw = base64.b64decode(text.strip())
    # Sparkle's old format stored 64 bytes (seed then public key) or 96; the seed is first in both.
    if len(raw) in (64, 96):
        raw = raw[:32]
    return raw


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    command = argv[1]
    if command == "keygen" and len(argv) == 3:
        seed = os.urandom(32)
        fd = os.open(argv[2], os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write(base64.b64encode(seed).decode() + "\n")
        print(base64.b64encode(public_key(seed)).decode())
        return 0
    if command == "public" and len(argv) == 3:
        print(base64.b64encode(public_key(_read_seed(argv[2]))).decode())
        return 0
    if command == "sign" and len(argv) == 4:
        with open(argv[3], "rb") as f:
            print(base64.b64encode(sign(_read_seed(argv[2]), f.read())).decode())
        return 0
    if command == "verify" and len(argv) == 5:
        with open(argv[3], "rb") as f:
            ok = verify(base64.b64decode(argv[2]), f.read(), base64.b64decode(argv[4]))
        print("valid" if ok else "INVALID")
        return 0 if ok else 1
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
