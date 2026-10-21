# Shard images (artifacts)

Format version 1. An artifact is the self-contained description of one
shard: populations, synaptic rows and a subscription index. The host
compiler writes it (`dbrain compile`), the recipe generator builds the
same format in RAM, `artifact_parse()` validates every field before a
node simulates it, and ESP32-class boards memory-map it straight from
flash.

## Layout

Little-endian, every section 4-byte aligned, the image itself 4-byte
aligned in memory.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `"DBRA"` |
| 4 | 2 | artifact format version (1) |
| 6 | 2 | model version (1) |
| 8 | 2 | shard id |
| 10 | 2 | shard version |
| 12 | 2 | neuron count `n` |
| 14 | 2 | population count `p` |
| 16 | 4 | row count `s` (= subscription entries) |
| 20 | 4 | row stream length `r` in 32-bit words |
| 24 | 4 | total image size in bytes |
| 28 | 4 | CRC-32 (IEEE) of the image with this field zero |
| 32 | 48·p | population parameter blocks |
| | 2·(p+1), padded to 4 | `pop_first`: neuron `i` belongs to population `k` iff `pop_first[k] ≤ i < pop_first[k+1]` |
| | 4·r | row stream |
| | 12·s | subscription index |

### Population block (48 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | model: 0 Izhikevich, 1 LIF |
| 1 | 1 | reserved |
| 2 | 2 | refractory ticks (LIF) |
| 4 | 4 × 4 | `a, b, c, d` (s16.15) |
| 20 | 4 | `i_bias` |
| 24 | 4 × 5 | `v_rest, v_reset, v_thresh, alpha, beta` |
| 44 | 4 | reserved, zero |

### Row stream

A sequence of rows in ascending source gid: `u32 gid`, `u32 length in
words`, then the segments of [simulation-core.md §4](simulation-core.md#4-synapses-and-delivery).
A gid is `(shard id << 16) | local index`; on-board sources use the
image's own shard id.

### Subscription index

One entry per row, ascending by gid: `u32 gid`, `u32 offset of the first
segment word`, `u16 length in words`, `u16 reserved`. Receivers binary
search it per event; `artifact_subscribes(shard)` finds whether any row
comes from a shard in `O(log s)`.

## Validation

`artifact_parse` rejects, with a distinct error code each: wrong
alignment, size, magic, format version, model version, CRC; populations
that are empty, unordered or of unknown model; rows whose segments have a
zero count, a weight shift above 15, a delay of 0 or a target outside
`n`; a subscription index that does not describe the row stream exactly.
Images that pass need no bounds checks in the hot path.

## Sizes

| Content | Bytes |
|---|---|
| header | 32 |
| population | 48 |
| row with `k` synapses in `g` segments | 8 + 4·g + 4·k, plus 12 for its index entry |

A 1000-neuron shard with 100 synapses per source neuron in two segments is
about 1000 × (8 + 8 + 400 + 12) ≈ 430 KB.

## Builder rules

Both builders (`artifact_build_*` in C and `ArtifactBuilder` in Python)
enforce: populations before rows; rows in strictly ascending gid; targets
non-decreasing within a row; delays 1..15; weights in int16; no empty
rows. Both produce identical bytes for identical input; the test suite
builds every recipe topology with both and compares.

## On a board

ESP32 family: partition `artifact` (type `data`, subtype `0x40`) is
memory-mapped read-only; the arena for state and buckets lives in
internal RAM. Write an image with
`dbrain flash-artifact --port … --chip … shard_N.bin` or transfer it
over the control plane (`ART_BEGIN/CHUNK/END`) into the image buffer and
persist it with the console command `artifact save`.

ESP8266: the image is copied from its 64 KB partition into the image
buffer at boot (`CONFIG_DBRAIN_IMAGE_KB`, 16 KB by default), so shards for
an ESP8266 must stay small.
