"""xorshift32 with the same seeding and reduction as core/src/rng.c."""

M32 = 0xFFFFFFFF


def mix(x: int) -> int:
    x &= M32
    x ^= x >> 16
    x = (x * 0x85EBCA6B) & M32
    x ^= x >> 13
    x = (x * 0xC2B2AE35) & M32
    x ^= x >> 16
    return x


class Rng:
    __slots__ = ("s",)

    def __init__(self, a: int, b: int = 0, c: int = 0):
        s = mix((a ^ 0x9E3779B9) & M32)
        s = mix((s ^ b ^ 0x7F4A7C15) & M32)
        s = mix((s ^ c ^ 0x6A09E667) & M32)
        self.s = s if s else 0x2545F491

    def next(self) -> int:
        x = self.s
        x ^= (x << 13) & M32
        x ^= x >> 17
        x ^= (x << 5) & M32
        self.s = x
        return x

    def below(self, n: int) -> int:
        return (self.next() * n) >> 32

    def unit(self) -> int:
        return self.next() >> 17
