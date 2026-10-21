"""Compilerless networks: the same derivation as core/src/recipe.c."""

import struct
from dataclasses import dataclass
from typing import List, Tuple

from . import fix
from .artifact import ArtifactBuilder, WEIGHT_SHIFT_DEFAULT, gid
from .neuron import FS, RS
from .rng import Rng, mix

RANDOM, SPATIAL, SYNFIRE = 0, 1, 2
LEN = 16


@dataclass
class Recipe:
    seed: int = 1
    topology: int = RANDOM
    exc_percent: int = 80
    w_exc: int = 64
    w_inh: int = -128
    local_fan_in: int = 20
    remote_fan_in: int = 10
    drive: int = 12 * 128

    def pack(self) -> bytes:
        return struct.pack("<IBBhhHHH", self.seed, self.topology, self.exc_percent, self.w_exc, self.w_inh,
                           self.local_fan_in, self.remote_fan_in, self.drive)

    @classmethod
    def unpack(cls, data: bytes) -> "Recipe":
        return cls(*struct.unpack("<IBBhhHHH", data[:LEN]))


def exc_count(r: Recipe, n: int) -> int:
    return (n * r.exc_percent) // 100


def _node_coord(seed: int, node_id: int) -> Tuple[int, int]:
    h = mix(seed ^ ((node_id * 0x9E3779B1) & 0xFFFFFFFF))
    return h & 1023, (h >> 10) & 1023


def _isqrt(v: int) -> int:
    d = 0
    bit = 1 << 20
    while bit > v:
        bit >>= 2
    while bit:
        if v >= d + bit:
            v -= d + bit
            d = (d >> 1) + bit
        else:
            d >>= 1
        bit >>= 2
    return d


def _spatial_factor(seed: int, a: int, b: int) -> int:
    ax, ay = _node_coord(seed, a)
    bx, by = _node_coord(seed, b)
    d = _isqrt((ax - bx) ** 2 + (ay - by) ** 2)
    return 0 if d >= 768 else ((768 - d) * 256) // 768


def _fan_out(r: Recipe, src: int, n_src: int, dst: int, n_dst: int, n_members: int, src_rank: int,
             dst_rank: int) -> int:
    if src == dst:
        fan_in = r.local_fan_in
    elif r.topology == SYNFIRE:
        if n_members < 2 or (src_rank + 1) % n_members != dst_rank:
            return 0
        fan_in = r.remote_fan_in
    else:
        if n_members < 2:
            return 0
        fan_in = (r.remote_fan_in + (n_members - 2)) // (n_members - 1)
        if r.topology == SPATIAL:
            fan_in = (fan_in * _spatial_factor(r.seed, src, dst) + 128) // 256
    if fan_in == 0 or n_src == 0:
        return 0
    return (fan_in * n_dst + n_src // 2) // n_src


def build(r: Recipe, self_id: int, self_version: int, members: List[Tuple[int, int]]) -> bytes:
    """members: sorted (node_id, n_neurons) including self. Returns the image bytes."""
    n_self = None
    self_rank = 0
    for i, (nid, n) in enumerate(members):
        if i > 0 and nid <= members[i - 1][0]:
            raise ValueError("members must ascend by id")
        if nid == self_id:
            n_self, self_rank = n, i
    if not n_self:
        raise ValueError("self not in members")
    exc_self = exc_count(r, n_self)
    b = ArtifactBuilder(self_id, self_version)
    if exc_self > 0:
        b.add_population(RS, exc_self)
    if n_self > exc_self:
        b.add_population(FS, n_self - exc_self)
    for k, (src, n_src) in enumerate(members):
        exc_src = exc_count(r, n_src)
        fo = _fan_out(r, src, n_src, self_id, n_self, len(members), k, self_rank)
        if fo == 0:
            continue
        fo = min(fo, 255)
        delay_min = 1 if src == self_id else 2
        delay_span = 9 - delay_min
        for i in range(n_src):
            g = Rng(r.seed, ((src << 16) | i) & 0xFFFFFFFF, self_id)
            targets = sorted(g.below(n_self) for _ in range(fo))
            w = r.w_exc if i < exc_src else r.w_inh
            b.row_begin(gid(src, i), WEIGHT_SHIFT_DEFAULT)
            for t in targets:
                b.synapse(t, w, delay_min + g.below(delay_span), 0)
            b.row_end()
    return b.finish()


def drive(r: Recipe, shard_id: int, tick: int, n_neurons: int) -> List[int]:
    """Per-tick background input of every neuron, in s16.15."""
    if r.drive == 0:
        return [0] * n_neurons
    g = Rng((r.seed ^ 0xD71F) & 0xFFFFFFFF, shard_id, tick & 0xFFFFFFFF)
    span = r.drive << 8
    return [g.below(span) for _ in range(n_neurons)]
