# DBrain system architecture

This document describes the software as built. The design rationale and the
measurements behind every number are in [research.md](research.md); the
byte-level wire formats are in [protocol.md](protocol.md); the numerical
scheme of the simulation is in [simulation-core.md](simulation-core.md).

## 1. Repository layout

| Path | Contents | Builds with |
|---|---|---|
| `core/` | Portable C99 core: simulation, wire formats, membership, election, clock sync, slots, control plane, recipe networks, the node agent. No OS calls, no dynamic allocation in the running system. | Host CMake, ESP-IDF, ESP8266 RTOS SDK |
| `firmware/esp32/` | ESP-IDF v5.5 project for ESP32, ESP32-S2, S3, C2, C3, C5, C6 | `idf.py` |
| `firmware/esp8266/` | ESP8266 RTOS SDK v3.4 project | `idf.py` of that SDK |
| `firmware/dbrain_common/` | Console commands, gateway stream, transport benchmark shared by both firmware projects | as a component of either |
| `sim/` | Virtual radio cell and the `dbrain-sim` scenario runner: real node agents on the host | Host CMake |
| `tests/` | C unit and integration tests (ctest) | Host CMake |
| `host/` | Python package `dbrain`: network compiler, reference simulator, gateway, flashing, bit-exact mirrors of the core | `pip install -e host` |
| `docs/` | This documentation | |

The core is compiled three ways from one directory: `core/CMakeLists.txt`
registers itself as an ESP-IDF component when `idf_component_register`
exists, as a legacy component when `register_component` exists (ESP8266
RTOS SDK), and as a static plus shared library otherwise.

## 2. One node

A node is one board. Everything a board does is implemented in the core as
`node_t` ([core/include/dbrain/node.h](../core/include/dbrain/node.h)); the
firmware only supplies a platform:

```
node_platform_t
  now_us()        monotonic microsecond clock
  send(mac, frame, len, tag)   mac NULL = broadcast; tag echoed by node_on_sent
  get_mac(), get_cap()        identity and capability record
  random()                    jitter source (not used by the simulation)
  log(level, text), event(node_event_t)
```

and drives the agent with three calls:

| Call | When |
|---|---|
| `node_poll(n)` | every millisecond or more often: timers, tick loop, control-plane retries |
| `node_on_frame(n, src_mac, data, len, rx_us, hw_ts)` | every received frame, with the receive timestamp |
| `node_on_sent(n, tag, tx_us)` | when a frame has left the air (needed for beacons) |

Inside the agent:

```
            heartbeats / beacons / control frames / spike bundles
                     |              |              |
               membership_t    timesync_t      ctrl_t          (core modules)
                     |              |              |
                     +------- node_t state machine -------+
                                     |
                                  shard_t  <-- artifact image (compiled or from a recipe)
                                     |
                          neuron_step() over every neuron, every tick
```

### 2.1 States

| State | Meaning | Leaves when |
|---|---|---|
| `DISCOVER` | listening; heartbeats carry node id 0 | a coordinator is heard (to `JOINING`), or after `discover_ms` with nobody leading and the node eligible (to `COORDINATOR`) |
| `JOINING` | `JOIN_REQ` in flight to the coordinator | `JOIN_ACK` arrives (to `MEMBER`); after 31 retries (back to `DISCOVER`) |
| `MEMBER` | has a node id, follows the coordinator's beacons, runs its shard | the coordinator falls silent for 3.5 s (to `DISCOVER`, then possibly `COORDINATOR`), or a better coordinator appears (to `JOINING` under the new one) |
| `COORDINATOR` | beacons time, admits joiners, numbers the epoch, runs its shard | it hears a claim with a higher (epoch, score, MAC) key (yields, re-joins) |

Promotion is ordered: a node that would self-promote waits
`300 ms × (number of live eligible peers with a better key)` first, so on a
cold start the strongest board forms the cluster and the others join it.

### 2.2 Capability and tiers

Each node advertises a 16-byte capability record (chip, cores, MHz, free
heap, PSRAM, flags). `cap_classify()` derives the tier and eligibility:

| Chip | Tier | Coordinator eligible | Hardware receive timestamp |
|---|---|---|---|
| ESP8266, ESP32-C2 | leaf | no | ESP8266: no; C2: yes |
| ESP32-S2, C3, C5, C6 | mid | yes | yes |
| ESP32, ESP32-S3 | workhorse (heavy with PSRAM) | yes | yes |

The election key is `(epoch, cap_score, MAC)`; `cap_score` packs tier,
PSRAM megabytes, MHz, cores and chip id (never free heap, so the key does
not move while a node runs).

## 3. The cluster

### 3.1 Membership

Every node broadcasts a heartbeat once per second (±100 ms jitter). Every
node keeps a table of up to `MEMBERSHIP_MAX` (64; 32 on the ESP8266)
members keyed by MAC. Liveness:

| Silence | Liveness | Action |
|---|---|---|
| < 3 s | alive | |
| ≥ 3 s | suspect | one direct `PING` plus `PROBE_REQ` to two live peers, who answer with their own view |
| 2 s after suspicion without an answer | dead | removed from slot ranking and recipe rosters; control messages to it are cancelled |
| 30 s after death | forgotten | its node id becomes free again |

### 3.2 Time

Cluster time is the coordinator's microsecond clock. Tick `N` starts at
`tick_origin_us + N × tick_period_us`; both come from the beacon. Members
estimate `(coordinator − local)` by an 8-point linear regression over
beacon samples (receive timestamp of beacon `k` paired with the transmit
time of beacon `k` reported in a later beacon), slew the applied offset by
at most 10 µs per millisecond, and step only when the error exceeds a
whole tick. See [protocol.md §6](protocol.md#6-synchronisation).

Measured in the simulator with ±10 ppm crystals and an 80 µs receiver
spread: estimate error under 60 µs; with 1 ms software timestamps
(ESP8266 class) under 1.5 ms.

### 3.3 Slots

Each tick is divided into an integration budget (1000 µs) and `S + 1`
transmission slots; slot 0 is a contention window for beacons, heartbeats
and control traffic, slots `1..S` belong to the members in ascending
node-id order. A member sends its tick's spike bundle at the start of its
slot. Slot length is the airtime of a bundle carrying `slot_events` events
plus a guard (100 µs, or 1500 µs when any member lacks a hardware
timestamp). The tick period is `1000 + (S + 1) × slot_len`, never below the
configured minimum. Worked examples are in
[protocol.md §7](protocol.md#7-slot-plan).

### 3.4 Spikes

A node integrates tick `T`, collects the neurons that fired, queues their
on-board synapses, and sends the fired indices as one or more bundles
tagged with `T`. A receiver that holds synapses from the sender's shard
looks each index up in its subscription table and adds every synapse's
weight into the bucket of tick `T + delay`. Bundles are never
acknowledged or retransmitted; sequence gaps are counted. The acceptance
window `tick ≤ T + delay ≤ tick + 15` tolerates one tick of skew in either
direction with the minimum cross-node delay of 2.

### 3.5 Where the network comes from

| Mode | Source of the shard | Node id ↔ shard id |
|---|---|---|
| recipe | 16-byte recipe broadcast by the coordinator in `JOIN_ACK`; every node derives its own population and rows from the recipe and the roster | shard id = node id |
| artifact | image compiled by `dbrain compile` and written to the board's artifact partition (or transferred over the control plane) | shard id chosen by the compiler (= node id of the board it was compiled for) |

In recipe mode the roster signature (ids and sizes of recipe members)
triggers a rebuild of the shard; when the node's own population is
unchanged the rebuild is warm: neuron state, buckets and tick counter
survive.

## 4. Memory on a board

| Item | Size | Where |
|---|---|---|
| neuron state | 8 B × n | internal RAM (arena) |
| delay buckets | 16 × 4 B × n | internal RAM (arena) |
| spike list | 2 B × n | internal RAM (arena) |
| synaptic rows + subscription index | 4 B per synapse, 4 B per segment, 20 B per source row | artifact partition (memory-mapped flash) or the image buffer (PSRAM when present, else internal) |
| `node_t` | ≈ 7 KB (64-entry table) | static |

`CONFIG_DBRAIN_MAX_NEURONS` sizes the arena; `CONFIG_DBRAIN_IMAGE_KB`
sizes the image buffer. Recipe sizes are capped by both the capability
budget and the arena.

## 5. Failure behaviour

| Event | Effect |
|---|---|
| a member dies | coordinator replans slots; recipe nodes drop its rows; its spikes stop; nothing else changes |
| the coordinator dies | members detect 3.5 s of beacon silence; the best eligible member promotes with epoch + 1, continuing the tick count it last saw; the rest re-join and keep their ids |
| a partition | each side elects its own coordinator; cross-partition spikes are counted losses; on merge the higher epoch wins, then the higher key, and the losing side re-joins |
| a node restarts | it is admitted again with its previous id for 30 s after its death |
| a late or early bundle | counted (`events_late`, `events_early`), never applied at the wrong tick |

## 6. What is not in this release

- plasticity (the 64-bit plastic synapse layout is reserved, not implemented),
- paced mode (per-tick acknowledgement); `pause`/`resume` and `FENCE` exist,
- over-the-air firmware update, application-layer encryption,
- runtime respawn of a dead node's compiled shard without the host.
