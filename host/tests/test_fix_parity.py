import random

from dbrain import fix
from dbrain.core import load


def test_selftest_and_constants():
    L = load()
    assert L.fix_selftest()
    assert fix.ratio(4, 100) == 1311
    assert fix.ratio(-1, 2) == -16384
    assert fix.from_float(0.04) == 1311
    assert fix.from_float(-65) == -65 * 32768


def test_mul_matches_c():
    L = load()
    rng = random.Random(7)
    pairs = [(rng.randint(-2**31, 2**31 - 1), rng.randint(-2**31, 2**31 - 1)) for _ in range(20000)]
    pairs += [(fix.ONE, fix.ONE), (1, fix.HALF), (-1, fix.HALF), (fix.MAX, fix.MAX), (fix.MIN, fix.MAX)]
    for a, b in pairs:
        assert fix.mul(a, b) == L.fix_mul_fn(a, b), (a, b)


def test_saturation():
    assert fix.add_sat(fix.MAX, 1) == fix.MAX
    assert fix.sub_sat(fix.MIN, 1) == fix.MIN
    assert fix.mul_sat(fix.from_int(300), fix.from_int(300)) == fix.MAX
