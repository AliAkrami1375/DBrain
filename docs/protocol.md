# DBrain wire protocol, version 1

Every frame is an ESP-NOW v1 payload of at most 250 bytes so that every
supported chip, including the ESP8266, can receive every frame. All
multi-byte fields are little-endian and are read and written byte-wise;
frames are never cast to structs. Implementation: `core/src/{bundle,
membership, timesync, control}.c`.

## 1. Preamble

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | magic `0xDB` |
| 1 | 1 | high nibble protocol version (`1`), low nibble frame type |

Frame types: `1` bundle, `2` heartbeat, `3` beacon, `4` control, `15`
benchmark (ignored by the node). Any other type or version is dropped.

## 2. Spike bundle (type 1, broadcast)

| Offset | Size | Field |
|---|---|---|
| 2 | 2 | source node id |
| 4 | 4 | tick in which the events fired |
| 8 | 2 | per-node bundle sequence number |
| 10 | 1 | event count `n`, 0..119 |
| 11 | 1 | flags: bit 0 `MORE` (another bundle for the same tick follows) |
| 12 | 2n | events: local neuron indices on the source shard |

12-byte header plus 119 events fills 250 bytes. A bundle is accepted when
its sender's MAC is a live or suspect member whose node id equals the
source node id; the receiver then checks whether it holds synapses from
that member's shard (`shard_subscribes`). Sequence numbers are per sender;
a gap of `g` counts `g` lost bundles; a backwards jump of more than 32768
means the sender restarted.

## 3. Heartbeat (type 2, broadcast, 56 bytes)

Sent every `1000 ± 100` ms by every node in every state after boot.

| Offset | Size | Field |
|---|---|---|
| 2 | 2 | cluster id |
| 4 | 2 | node id (0 before admission) |
| 6 | 6 | MAC |
| 12 | 2 | epoch the node follows (0 if none) |
| 14 | 2 | coordinator node id it follows (0 if none) |
| 16 | 2 | shard id (`0xFFFF` none) |
| 18 | 2 | shard version |
| 20 | 2 | neuron count of the shard (or the size the node will take in recipe mode) |
| 22 | 16 | capability record (§3.1) |
| 38 | 1 | node state: 0 boot, 1 discover, 2 joining, 3 member, 4 coordinator |
| 39 | 1 | flags: bit 0 coordinator, bit 1 has shard, bit 2 runs the recipe |
| 40 | 2 | heartbeat sequence |
| 42 | 4 | current tick |
| 46 | 2 | bundles lost (saturating) |
| 48 | 2 | tick overruns (saturating) |
| 50 | 2 | last sync residual in µs (int16) |
| 52 | 4 | reserved, zero |

### 3.1 Capability record (16 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | chip: 1 ESP8266, 2 ESP32, 3 S2, 4 S3, 5 C2, 6 C3, 7 C5, 8 C6, 15 host |
| 1 | 1 | cores |
| 2 | 2 | MHz |
| 4 | 4 | free internal heap, bytes |
| 8 | 4 | PSRAM, bytes |
| 12 | 1 | flags: bit 0 FPU, bit 1 hardware receive timestamp, bit 2 coordinator eligible, bit 3 PSRAM |
| 13 | 1 | tier: 0 leaf, 1 mid, 2 workhorse, 3 heavy |
| 14 | 2 | reserved |

`cap_score = tier<<28 | min(psram_MB,255)<<20 | min(MHz,4095)<<8 | min(cores,15)<<4 | chip`.

## 4. Beacon (type 3, broadcast, 60 bytes)

Sent by the coordinator every 1000 ms, and at once after a slot plan
change. Never more than one beacon is in flight: the next one waits for
the send callback of the previous.

| Offset | Size | Field |
|---|---|---|
| 2 | 2 | cluster id |
| 4 | 2 | epoch |
| 6 | 2 | coordinator node id |
| 8 | 6 | coordinator MAC |
| 14 | 2 | beacon sequence |
| 16 | 2 | `tx_latency_us`: send-callback latency receivers subtract |
| 18 | 2 | member count `S` |
| 20 | 8 | `prev_tx_time_us`: coordinator time when beacon `prev_tx_seq` left the air (0 unknown) |
| 28 | 8 | `tick_origin_us`: coordinator time of the start of tick 0 |
| 36 | 4 | `tick_period_us` |
| 40 | 4 | coordinator's tick when the beacon was built |
| 44 | 4 | slot plan version |
| 48 | 2 | slot length, µs |
| 50 | 2 | events a slot is sized for |
| 52 | 1 | PHY rate index: 0 = 1 Mbps, 1 = 6, 2 = 24, 3 = 54 |
| 53 | 1 | flags: bit 0 paced (reserved), bit 1 recipe mode |
| 54 | 2 | `prev_tx_seq` |
| 56 | 4 | coordinator `cap_score` |

## 5. Control (type 4, unicast)

| Offset | Size | Field |
|---|---|---|
| 2 | 2 | cluster id |
| 4 | 2 | source node id (0 before admission) |
| 6 | 2 | destination node id (0 when addressed by MAC only) |
| 8 | 1 | message type |
| 9 | 1 | flags: bit 0 `ACK` (this frame acknowledges `seq`), bit 1 `REQ_ACK` |
| 10 | 1 | sequence (per sender, wraps at 256) |
| 11 | 1 | payload length ≤ 238 |
| 12 | n | payload |

Reliable messages carry `REQ_ACK`; the receiver answers with an empty
frame of the same type and sequence with `ACK` set. The sender
retransmits every 100 ms, up to 31 tries (≈ 3 s), then reports failure.
Receivers remember the last 8 `(MAC, seq)` pairs and acknowledge
duplicates without handling them; handlers are idempotent regardless.

| Type | Name | Reliable | Payload |
|---|---|---|---|
| 1 | `JOIN_REQ` | yes | capability (16), MAC (6), preferred node id u16, held shard id u16, neuron count u16 — 28 bytes |
| 2 | `JOIN_ACK` | yes | node id u16, epoch u16, coordinator id u16, mode u8 (0 recipe, 1 artifact), pad u8, recipe (16) — 24 bytes |
| 3 | `PING` | no | empty |
| 4 | `PONG` | no | empty |
| 5 | `PROBE_REQ` | no | MAC of the suspect (6) |
| 6 | `PROBE_RSP` | no | MAC (6), alive u8 |
| 7 | `ASSIGN` | yes | shard id u16, shard version u16, source u8, pad (reserved for runtime reassignment) |
| 8 | `FENCE` | yes | action u8 (0 pause, 1 resume), pad u8, tick u32 |
| 9 | `TELEMETRY_REQ` | no | empty |
| 10 | `TELEMETRY_RSP` | no | `telemetry_t` as 22 little-endian u32 (88 bytes) |
| 11 | `ART_BEGIN` | yes | shard id u16, shard version u16, size u32, CRC-32 u32 |
| 12 | `ART_CHUNK` | yes | offset u32, data (≤ 234 bytes) |
| 13 | `ART_END` | yes | CRC-32 u32 |
| 14 | `ART_STATUS` | no | status u8: 0 ok, 1 busy, 2 crc, 3 no space, 4 rejected |
| 15 | `INJECT` | no | neuron u16, pad u16, current s16.15 i32, tick u32 (0 = next) |

Node id assignment by the coordinator: an existing live entry keeps its
id; a dead entry younger than 30 s gets its old id back; otherwise the
preferred id if free, else the lowest free id in 1..4095.

### 5.1 Recipe (16 bytes, inside `JOIN_ACK`)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | seed |
| 4 | 1 | topology: 0 random, 1 spatial, 2 synfire |
| 5 | 1 | excitatory percent, 0..100 |
| 6 | 2 | excitatory weight, int16 at shift 8 (128 = 1.0) |
| 8 | 2 | inhibitory weight |
| 10 | 2 | on-board synapses per neuron |
| 12 | 2 | off-board synapses per neuron, in total |
| 14 | 2 | peak of the per-tick random drive, 1/128 units |

The derivation of a shard from a recipe is specified in
[simulation-core.md §5](simulation-core.md#5-recipe-networks).

## 6. Synchronisation

1. The coordinator sends beacon `k`. Its send callback records the local
   time `tx_k` at which the frame left the air.
2. The next beacon (sequence `k'`) carries `prev_tx_seq = k` and
   `prev_tx_time_us = tx_k`.
3. A receiver that stored the hardware receive timestamp `rx_k` of beacon
   `k` forms the sample `(rx_k, tx_k − tx_latency_us)`. Samples whose
   sequence does not match are ignored.
4. The last 8 samples feed an integer least-squares fit of
   `offset = coordinator − local` against local time (slope capped at
   1 %, window 60 s, a jump above 1 s resets the filter).
5. Every millisecond the member moves its applied offset toward the
   estimate by at most 10 µs; an error above one tick period is applied
   at once and counted as a resync.
6. Cluster time is `local + applied offset`; the current tick is
   `(cluster − tick_origin) / tick_period`.

All receivers hear a broadcast within ≈ 80 µs of each other, while the
coordinator's send-callback latency is a constant that shifts the
coordinator alone; `tx_latency_us` lets a measured value remove it.

## 7. Slot plan

```
slot_len   = airtime(12 + 2 × slot_events bytes) + guard
period     = max(min_period, 1000 µs + (S + 1) × slot_len)
slot i     starts at 1000 µs + i × slot_len after the tick start
member rank r (ascending node id among active members) uses slot r + 1
```

Airtime model (`slot_airtime_us`): 39 bytes of frame overhead around the
payload; 1 Mbps DSSS: 192 µs preamble + bits + 50 µs DIFS + 310 µs mean
backoff; OFDM rates: 24 µs preamble + bits/rate + 28 µs DIFS + 140 µs mean
backoff.

| Members | Rate | slot_events | Guard | Slot | Period |
|---|---|---|---|---|---|
| 2 | 1 Mbps | 32 | 100 µs | 1572 µs | 5716 µs |
| 4 | 1 Mbps | 32 | 100 µs | 1572 µs | 8860 µs |
| 2 + ESP8266 | 1 Mbps | 32 | 1500 µs | 2972 µs | 12 888 µs |
| 40 | 24 Mbps | 32 | 100 µs | 230 µs | 10 430 µs |

Any ESP8266 member forces 1 Mbps; any member without a hardware receive
timestamp forces the 1500 µs guard. A bundle that does not fit the slot
(more than `slot_events` spikes) is still sent at the slot start; its
`MORE` continuations follow immediately under CSMA.
