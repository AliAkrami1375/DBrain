"""Shard image builder and parser, byte-identical to core/src/artifact.c."""

import struct
import zlib
from dataclasses import dataclass, field
from typing import Dict, Iterator, List, Optional, Tuple

from . import ARTIFACT_VERSION, MODEL_VERSION
from .neuron import PARAMS_LEN, Params

MAGIC = b"DBRA"
HDR_LEN = 32
MAX_NEURONS = 65535
MAX_POPS = 4096
DELAY_MIN = 1
DELAY_MAX = 15
XNODE_DELAY_MIN = 2
SEG_MAX_COUNT = 255
WEIGHT_SHIFT_DEFAULT = 8
WEIGHT_SHIFT_MAX = 15
SUB_LEN = 12


def gid(shard: int, local: int) -> int:
    return (shard << 16) | local


def gid_shard(g: int) -> int:
    return g >> 16


def gid_local(g: int) -> int:
    return g & 0xFFFF


def syn_pack(weight: int, delay: int, flags: int, target_off: int) -> int:
    return ((weight & 0xFFFF) << 16) | ((delay & 0xF) << 12) | ((flags & 0xF) << 8) | (target_off & 0xFF)


def syn_unpack(w: int) -> Tuple[int, int, int, int]:
    weight = (w >> 16) & 0xFFFF
    if weight & 0x8000:
        weight -= 0x10000
    return weight, (w >> 12) & 0xF, (w >> 8) & 0xF, w & 0xFF


def seg_pack(base: int, count: int, shift: int) -> int:
    return (base << 16) | (count << 8) | shift


def seg_unpack(h: int) -> Tuple[int, int, int]:
    return h >> 16, (h >> 8) & 0xFF, h & 0xFF


def weight_accum(weight: int, shift: int) -> int:
    return weight << shift


class ArtifactError(ValueError):
    pass


class ArtifactBuilder:
    """Populations first, then rows by ascending gid with targets non-decreasing."""

    def __init__(self, shard_id: int, shard_version: int = 1):
        self.shard_id = shard_id
        self.shard_version = shard_version
        self.pops: List[Tuple[Params, int]] = []
        self.n_neurons = 0
        self.rows: List[int] = []          # row stream words
        self.n_rows = 0
        self._rows_started = False
        self._in_row = False
        self._last_gid = -1
        self._row_hdr = 0
        self._seg_hdr: Optional[int] = None
        self._seg_base = 0
        self._seg_count = 0
        self._shift = WEIGHT_SHIFT_DEFAULT
        self._last_target = 0

    def add_population(self, params: Params, count: int) -> int:
        if self._rows_started:
            raise ArtifactError("populations must precede rows")
        if count <= 0 or self.n_neurons + count > MAX_NEURONS or len(self.pops) >= MAX_POPS:
            raise ArtifactError("bad population size")
        first = self.n_neurons
        self.pops.append((params, count))
        self.n_neurons += count
        return first

    def row_begin(self, g: int, weight_shift: int = WEIGHT_SHIFT_DEFAULT) -> None:
        if self._in_row:
            raise ArtifactError("row already open")
        if not self.pops:
            raise ArtifactError("no populations")
        if weight_shift > WEIGHT_SHIFT_MAX:
            raise ArtifactError("weight shift")
        if self._rows_started and g <= self._last_gid:
            raise ArtifactError("rows must ascend by gid")
        self._rows_started = True
        self._row_hdr = len(self.rows)
        self.rows.append(g & 0xFFFFFFFF)
        self.rows.append(0)
        self._in_row = True
        self._seg_hdr = None
        self._seg_count = 0
        self._shift = weight_shift
        self._last_gid = g

    def synapse(self, target: int, weight: int, delay: int, flags: int = 0) -> None:
        if not self._in_row:
            raise ArtifactError("no row open")
        if not (DELAY_MIN <= delay <= DELAY_MAX) or not (0 <= target < self.n_neurons):
            raise ArtifactError("bad synapse")
        if not (-32768 <= weight <= 32767):
            raise ArtifactError("weight out of int16 range")
        if self._seg_hdr is not None and target < self._last_target:
            raise ArtifactError("targets must not decrease")
        if self._seg_hdr is None or self._seg_count == SEG_MAX_COUNT or target - self._seg_base > 0xFF:
            self._seg_hdr = len(self.rows)
            self._seg_base = target
            self._seg_count = 0
            self.rows.append(0)
        self.rows.append(syn_pack(weight, delay, flags, target - self._seg_base))
        self._seg_count += 1
        self.rows[self._seg_hdr] = seg_pack(self._seg_base, self._seg_count, self._shift)
        self._last_target = target

    def row_end(self) -> None:
        if not self._in_row:
            raise ArtifactError("no row open")
        if self._seg_hdr is None:
            raise ArtifactError("empty row")
        words = len(self.rows) - self._row_hdr - 2
        if words > 0xFFFF:
            raise ArtifactError("row too long")
        self.rows[self._row_hdr + 1] = words
        self._in_row = False
        self.n_rows += 1

    def finish(self) -> bytes:
        if self._in_row:
            raise ArtifactError("row still open")
        if not self.pops:
            raise ArtifactError("no populations")
        body = bytearray()
        for p, _ in self.pops:
            body += p.pack()
        first = 0
        pf = [0]
        for _, count in self.pops:
            first += count
            pf.append(first)
        pf_bytes = struct.pack("<%dH" % len(pf), *pf)
        pf_bytes += b"\0" * ((-len(pf_bytes)) % 4)
        body += pf_bytes
        rows_bytes = struct.pack("<%dI" % len(self.rows), *self.rows)
        body += rows_bytes
        subs = bytearray()
        pos = 0
        while pos < len(self.rows):
            g, length = self.rows[pos], self.rows[pos + 1]
            subs += struct.pack("<IIHH", g, pos + 2, length, 0)
            pos += 2 + length
        body += subs
        total = HDR_LEN + len(body)
        hdr = bytearray(struct.pack("<4sHHHHHHIIII", MAGIC, ARTIFACT_VERSION, MODEL_VERSION, self.shard_id,
                                    self.shard_version, self.n_neurons, len(self.pops), self.n_rows,
                                    len(self.rows), total, 0))
        crc = zlib.crc32(bytes(hdr) + bytes(body)) & 0xFFFFFFFF
        struct.pack_into("<I", hdr, 28, crc)
        return bytes(hdr) + bytes(body)


@dataclass
class Row:
    gid: int
    synapses: List[Tuple[int, int, int, int]] = field(default_factory=list)  # (target, weight, delay, flags)
    weight_shift: int = WEIGHT_SHIFT_DEFAULT


@dataclass
class Artifact:
    shard_id: int
    shard_version: int
    n_neurons: int
    pops: List[Tuple[Params, int]]
    pop_first: List[int]
    rows: Dict[int, Row]
    size: int

    @property
    def n_synapses(self) -> int:
        return sum(len(r.synapses) for r in self.rows.values())

    def subscribes(self, shard: int) -> bool:
        return any(gid_shard(g) == shard for g in self.rows)

    def sources(self) -> Iterator[int]:
        return iter(sorted({gid_shard(g) for g in self.rows}))


def parse(data: bytes) -> Artifact:
    """Validates and decodes an image (the C loader is the reference; this mirrors its checks)."""
    if len(data) < HDR_LEN:
        raise ArtifactError("size")
    magic, fmt, model, shard_id, version, n, p, n_subs, n_row_words, total, crc = struct.unpack_from(
        "<4sHHHHHHIIII", data, 0)
    if magic != MAGIC:
        raise ArtifactError("magic")
    if fmt != ARTIFACT_VERSION:
        raise ArtifactError("format version")
    if model != MODEL_VERSION:
        raise ArtifactError("model version")
    if total != len(data):
        raise ArtifactError("size")
    if zlib.crc32(data[:28] + b"\0\0\0\0" + data[HDR_LEN:]) & 0xFFFFFFFF != crc:
        raise ArtifactError("crc")
    if n == 0 or p == 0 or p > MAX_POPS:
        raise ArtifactError("populations")
    off = HDR_LEN
    pops = []
    for _ in range(p):
        pops.append(Params.unpack(data[off:off + PARAMS_LEN]))
        off += PARAMS_LEN
    pf = list(struct.unpack_from("<%dH" % (p + 1), data, off))
    off += ((p + 1) * 2 + 3) & ~3
    if pf[0] != 0 or pf[-1] != n or any(pf[i + 1] <= pf[i] for i in range(p)):
        raise ArtifactError("populations")
    rows_words = list(struct.unpack_from("<%dI" % n_row_words, data, off))
    off += n_row_words * 4
    subs = [struct.unpack_from("<IIHH", data, off + i * SUB_LEN) for i in range(n_subs)]
    off += n_subs * SUB_LEN
    if off != total:
        raise ArtifactError("size")
    rows: Dict[int, Row] = {}
    pos = 0
    k = 0
    last_gid = -1
    while pos < n_row_words:
        g, length = rows_words[pos], rows_words[pos + 1]
        if length == 0 or pos + 2 + length > n_row_words:
            raise ArtifactError("rows")
        if k >= n_subs or subs[k][0] != g or subs[k][1] != pos + 2 or subs[k][2] != length or g <= last_gid:
            raise ArtifactError("subscriptions")
        row = Row(g)
        i = pos + 2
        end = i + length
        while i < end:
            base, count, shift = seg_unpack(rows_words[i])
            i += 1
            if count == 0 or shift > WEIGHT_SHIFT_MAX or i + count > end:
                raise ArtifactError("rows")
            row.weight_shift = shift
            for j in range(count):
                w, d, f, t = syn_unpack(rows_words[i + j])
                if d < DELAY_MIN or base + t >= n:
                    raise ArtifactError("rows")
                row.synapses.append((base + t, w, d, f))
            i += count
        rows[g] = row
        last_gid = g
        k += 1
        pos += 2 + length
    if k != n_subs:
        raise ArtifactError("subscriptions")
    return Artifact(shard_id, version, n, list(zip(pops, [pf[i + 1] - pf[i] for i in range(p)])), pf, rows, total)
