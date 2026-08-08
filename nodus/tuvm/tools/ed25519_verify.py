# Copyright (c) 2026 Ekorau LLC
"""Vendored, dependency-free ed25519 *verify* (RFC 8032, Ed25519ph-less pure form).

rete#25 D4: the signature is checked by the RUNNER, outside the image it is
about to boot — so this file must run on a stock python3 with nothing
installed. It is verify-only on purpose: the runner has no business holding a
private key, and a signer here would be a mechanism nobody on this side needs.

Shape notes (they are the difference between "a reference" and "usable"):

* Field inversion is `pow(x, p-2, p)`, not the extended-Euclid ladder — one
  CPython big-int powmod instead of a Python loop.
* Points are EXTENDED coordinates (x, y, z, t), so the double-and-add ladder
  does no inversions at all; exactly two inversions happen per verify, both in
  the final affine compare. The naive affine reference does ~1000 and takes
  seconds. This one is a few milliseconds.
* The verification equation is the COFACTORLESS one, `[s]B == R + [h]A`, which
  is what Go's `crypto/ed25519.Verify` checks. Using the cofactored form here
  would accept signatures Go rejects, and the whole point of this file is to
  agree with the Go signer in `porta/internal/blob`.

Public surface: `verify(pub32: bytes, sig64: bytes, msg: bytes) -> bool`.
It never raises on malformed input — a bad key, a short signature or a
non-canonical point is a False, because every caller is a security decision
that must not turn into a traceback.
"""

import hashlib

__all__ = ["verify"]

# Curve constants (RFC 8032 §5.1).
p = 2 ** 255 - 19
q = 2 ** 252 + 27742317777372353535851937790883648493

_d = -121665 * pow(121666, p - 2, p) % p
_sqrt_m1 = pow(2, (p - 1) // 4, p)


def _inv(x):
    return pow(x, p - 2, p)


# Extended homogeneous coordinates: (X, Y, Z, T) with x = X/Z, y = Y/Z, xy = T/Z.
def _add(P, Q):
    A = (P[1] - P[0]) * (Q[1] - Q[0]) % p
    B = (P[1] + P[0]) * (Q[1] + Q[0]) % p
    C = 2 * P[3] * Q[3] * _d % p
    D = 2 * P[2] * Q[2] % p
    E, F, G, H = B - A, D - C, D + C, B + A
    return (E * F % p, G * H % p, F * G % p, E * H % p)


def _mul(s, P):
    """Double-and-add. Not constant time — this is a VERIFIER; every input it
    touches (the signature, the public key) is already public."""
    Q = (0, 1, 1, 0)          # the neutral element
    while s > 0:
        if s & 1:
            Q = _add(Q, P)
        P = _add(P, P)
        s >>= 1
    return Q


def _equal(P, Q):
    """Projective equality: X1/Z1 == X2/Z2 and Y1/Z1 == Y2/Z2, cross-multiplied."""
    if (P[0] * Q[2] - Q[0] * P[2]) % p != 0:
        return False
    if (P[1] * Q[2] - Q[1] * P[2]) % p != 0:
        return False
    return True


def _recover_x(y, sign):
    """The x matching this y and sign bit, or None if the point is not on the curve."""
    if y >= p:
        return None
    x2 = (y * y - 1) * _inv(_d * y * y + 1) % p
    if x2 == 0:
        # y == ±1: the only x is 0, and then the sign bit must be 0 or the
        # encoding is non-canonical.
        return None if sign else 0
    x = pow(x2, (p + 3) // 8, p)
    if (x * x - x2) % p != 0:
        x = x * _sqrt_m1 % p
    if (x * x - x2) % p != 0:
        return None
    if (x & 1) != sign:
        x = p - x
    return x


def _decompress(b):
    """32 little-endian bytes -> extended point, or None."""
    if len(b) != 32:
        return None
    y = int.from_bytes(b, "little")
    sign = y >> 255
    y &= (1 << 255) - 1
    x = _recover_x(y, sign)
    if x is None:
        return None
    return (x, y, 1, x * y % p)


_By = 4 * _inv(5) % p
_Bx = _recover_x(_By, 0)
_B = (_Bx, _By, 1, _Bx * _By % p)


def _sha512_modq(data):
    return int.from_bytes(hashlib.sha512(data).digest(), "little") % q


def verify(pub32, sig64, msg):
    """True iff sig64 is a valid ed25519 signature over msg under pub32.

    pub32 is the raw 32-byte public key (the runner's pub.key holds it as hex).
    msg is the RAW message — for #25 that is the 32 hash BYTES, never the hex
    text of them; `blob.SignHash` signs `hash[:]`.
    """
    if not isinstance(pub32, (bytes, bytearray)) or len(pub32) != 32:
        return False
    if not isinstance(sig64, (bytes, bytearray)) or len(sig64) != 64:
        return False
    pub32 = bytes(pub32)
    sig64 = bytes(sig64)

    A = _decompress(pub32)
    if A is None:
        return False
    Rs = sig64[:32]
    R = _decompress(Rs)
    if R is None:
        return False
    s = int.from_bytes(sig64[32:], "little")
    if s >= q:                      # a non-canonical scalar; Go rejects it too
        return False

    h = _sha512_modq(Rs + pub32 + bytes(msg))
    return _equal(_mul(s, _B), _add(R, _mul(h, A)))
