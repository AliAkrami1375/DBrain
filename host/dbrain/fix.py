"""s16.15 fixed-point arithmetic, identical to core/include/dbrain/fix.h."""

import math

FRAC_BITS = 15
ONE = 1 << FRAC_BITS
HALF = 1 << (FRAC_BITS - 1)
MAX = 2**31 - 1
MIN = -(2**31)


def wrap32(v: int) -> int:
    """Two's-complement truncation to 32 bits."""
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def clamp64(v: int) -> int:
    return MAX if v > MAX else MIN if v < MIN else v


def mul(a: int, b: int) -> int:
    """Round half up, then truncate like the C core (Python's >> is arithmetic)."""
    return wrap32((a * b + HALF) >> FRAC_BITS)


def mul_sat(a: int, b: int) -> int:
    return clamp64((a * b + HALF) >> FRAC_BITS)


def add_sat(a: int, b: int) -> int:
    return clamp64(a + b)


def sub_sat(a: int, b: int) -> int:
    return clamp64(a - b)


def clamp(a: int, lo: int, hi: int) -> int:
    return lo if a < lo else hi if a > hi else a


def from_int(i: int) -> int:
    return wrap32(i << FRAC_BITS)


def to_int(a: int) -> int:
    return a >> FRAC_BITS


def ratio(num: int, den: int) -> int:
    """FIX_RATIO: nearest, ties away from zero."""
    if den <= 0:
        raise ValueError("den must be positive")
    pos = (abs(num) * ONE * 2 + den) // (2 * den)
    return -pos if num < 0 else pos


def from_float(x: float) -> int:
    """Nearest s16.15 value, ties away from zero. Raises on overflow."""
    v = math.floor(abs(x) * ONE + 0.5)
    v = -v if x < 0 else v
    if v > MAX or v < MIN:
        raise OverflowError(f"{x} does not fit s16.15")
    return v


def to_float(a: int) -> float:
    return a / ONE
