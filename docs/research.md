# DBrain — Research and System Design

**Status:** pre-implementation research document
**Scope:** everything that must be understood and decided before the first line of firmware is written.

DBrain is a distributed spiking neural network (SNN) simulator built from a cluster of
ESP32-family and ESP8266 boards. Each board simulates a shard of neurons and synapses;
spikes travel between boards exclusively over ESP-NOW. The cluster is self-organizing:
boards discover each other, elect a coordinator, agree on time, and take on a share of the
network proportional to their capability. Boards can join or drop out while the simulation
runs.

This document surveys the prior art, pins down the hard numbers that constrain the design
(radio, memory, compute, clocks), and derives the architecture from them. Every load-bearing
number carries a source in [References](#12-references); numbers marked *estimate* are our
own arithmetic on top of sourced constants.

---

## Table of contents

1. [Vision, scope, honest scale](#1-vision-scope-honest-scale)
2. [Prior art](#2-prior-art)
3. [The transport: ESP-NOW in detail](#3-the-transport-esp-now-in-detail)
4. [Hardware survey: the cluster zoo](#4-hardware-survey-the-cluster-zoo)
5. [Neuron, synapse, and simulation model](#5-neuron-synapse-and-simulation-model)
6. [Spike transport design](#6-spike-transport-design)
7. [Time: synchronization and the tick](#7-time-synchronization-and-the-tick)
8. [Self-organization: discovery, election, placement](#8-self-organization-discovery-election-placement)
9. [Reliability, security, operations](#9-reliability-security-operations)
10. [Host tooling and visualization](#10-host-tooling-and-visualization)
11. [Risks, open questions, roadmap](#11-risks-open-questions-roadmap)
12. [References](#12-references)

---

## 1. Vision, scope, honest scale

### 1.1 What DBrain is

A wireless, cheap, incrementally scalable spike-level neural simulator. The architectural
ideas of the big neuromorphic machines — SpiNNaker's source-addressed multicast spikes,
Loihi's barrier-free logical time, TrueNorth's quantized global tick — reimplemented at
$3-per-node scale over a radio instead of a backplane. Add a board, the brain grows.

### 1.2 What DBrain is not

Honest calibration against biology, using the accepted reference numbers
[[bio-1]](#12-references) [[bio-2]](#12-references) [[bio-3]](#12-references):

| System | Neurons | Synapses |
|---|---|---|
| Human brain | 86 × 10⁹ | ~10¹⁴–10¹⁵ |
| Adult fruit fly (full connectome, 2024) | 139,255 | 54.5 × 10⁶ |
| *C. elegans* | 302 | ~7,600 |
| One SpiNNaker chip (18 cores, ~1 W) | ~10,000 real-time | ~10⁶–10⁷ |
| **DBrain, 10× ESP32 w/ PSRAM** *(estimate, §5.4)* | **~10⁴** | **~10⁶–10⁷** |

A ten-board cluster lands around 30× *C. elegans*, under a tenth of a fruit fly, and eight
orders of magnitude from a human brain — roughly one SpiNNaker chip's worth of neurons,
spread across a desk. The claim is "brain-inspired dynamics you can hold in your hands and
watch spike," not "brain-scale." Anything else would be dishonest, and dishonest framing is
how projects in this space have died (§2.3).

### 1.3 Hard requirements

- **R1** — Inter-board communication is ESP-NOW only. No router, no TCP/IP, no MQTT between nodes.
- **R2** — Nodes self-join: power a board near the cluster and it becomes part of the brain.
- **R3** — Work placement is automatic: the system decides which neurons live on which node.
- **R4** — The cluster keeps a shared time base tight enough for coordinated lock-step processing.
- **R5** — Heterogeneous by design: every ESP-NOW-capable ESP32 variant plus ESP8266 can
  participate, each contributing what it can. (Two family members physically cannot: see §4.)

---

## 2. Prior art

### 2.1 The big machines — where the ideas come from

**SpiNNaker** (Manchester) is the closest thing to a blueprint. Up to 1,036,800 ARM968
cores; each core simulates a few hundred neurons; a spike is a 40-bit packet carrying *only
the identity of the neuron that fired* — no destination. Routers replicate packets toward
subscriber cores using tables compiled offline from the network graph. Timestep is 1 ms of
wall clock; there is **no global barrier and no global clock** — cores that overrun a busy
tick catch up during quiet ones. Congested routers **drop packets rather than retry**,
counting drops as a health metric, because occasional spike loss is biologically tolerable
but stalling real time is not. Neuron state lives in fast local memory; the much larger
synapse matrix lives in off-chip SDRAM and is DMA-streamed only when a spike arrives.
[[spin-1]](#12-references) [[spin-2]](#12-references) [[spin-3]](#12-references)

Every one of those decisions transfers to a radio cluster, as later sections show. Notably,
SpiNNaker 2 moved to Cortex-M4F cores at a few hundred MHz — the same class of silicon as an
ESP32 [[spin-4]](#12-references).

**Intel Loihi / Loihi 2**: asynchronous cores; global time advances by *barrier
synchronization events*, not a clock — a timestep is a logical agreement, which is exactly
the right mental model for a network with jitter [[nm-1]](#12-references). **IBM TrueNorth**:
1 M neurons, global 1 ms tick, spike packets carry a 4-bit *delivery tick* so a jittery
transport can still deliver spikes at the correct logical time [[nm-2]](#12-references).
**BrainScaleS**: analog neurons 1,000–10,000× faster than biology; relevant here mainly as
the opposite pole of the design space [[nm-3]](#12-references).

### 2.2 Small projects — who has tried nearby things

- **Spikeling** (PLOS Biology): one ESP32 running an Izhikevich neuron in real time as a
  teaching instrument; multiple units interconnect through 3.5 mm audio jacks. Proof that
  real-time Izhikevich on this silicon is comfortable [[prior-1]](#12-references).
- **NeuroBytes**: physical PCB neurons joined by cables; NSF-granted, Kickstarter-funded,
  shipped kits, recreated a flatworm connectome — then folded around 2020. Lessons: people
  love tangible neurons; cost-per-neuron and cabling cap the scale at dozens; the open
  files outlived the company [[prior-2]](#12-references).
- **Lu.i** (Heidelberg): $3 analog LIF neuron PCB, active, wired, small-scale
  [[prior-3]](#12-references).
- **Broccoli**: MicroPython ESP32 cluster with a Celery-style task queue. Abandoned after
  the demo. Lessons: central-master orchestration plus no compelling workload is a dead
  end; the network was the bottleneck [[prior-4]](#12-references).
- **DDSNN** (2025): a conventional CNN partitioned layer-wise across wireless MCUs —
  nearest academic relative, but pipelined inference, not spiking, not ESP-NOW
  [[prior-5]](#12-references).

### 2.3 Gap analysis

Multiple search angles found **no existing project — hobby or academic — in which a cluster
of ESP32/ESP8266 boards exchanges spikes over ESP-NOW** (or any radio) to simulate one
neural network. The occupied niches are single-chip neuron instruments, wired neuron
boards, generic ESP32 clusters, and big-iron machines. The niche DBrain fills is real, and
the failure modes of the neighbors are documented: flooding meshes collapse under dense
traffic, JSON-per-message transports drown in overhead, and central masters plus no killer
demo lead to abandonment. The design below is shaped by those autopsies.

---

## 3. The transport: ESP-NOW in detail

ESP-NOW rides on 802.11 vendor-specific *action frames* — connectionless, no association,
no AP. Everything below is what the protocol actually guarantees, because the whole
architecture hangs off these numbers.

### 3.1 Frames and payloads

- Frame = 24 B MAC header + category/OUI/anti-replay (8 B) + vendor element (7 B header +
  payload) + 4 B FCS. Fixed overhead ≈ 39 B around the payload [[now-1]](#12-references).
- **v1 payload: 250 B** (`ESP_NOW_MAX_DATA_LEN`). **v2 payload: 1470 B**, a software
  feature of ESP-IDF ≥ 5.4 (early 5.4.x said 1490; corrected in 5.4.2)
  [[now-1]](#12-references) [[now-2]](#12-references).
- Mixed networks: v2 devices receive v1 frames; v1 devices receive v2 frames **only if the
  payload is ≤ 250 B**. ESP8266 is v1 forever (its SDK is frozen).
  **Design rule: any frame an ESP8266 might need to hear is capped at 250 B.** Larger
  frames are allowed only on traffic classes declared ESP32-only.

### 3.2 Peers, broadcast, and what "delivered" means

- Peer table: **20 peers max**. Encrypted peers: 7 by default, 17 max on ESP32-class
  (2/4 on C2; ESP8266: 10 in station mode, 6 in SoftAP) [[now-1]](#12-references)
  [[now-2]](#12-references).
- **Broadcast** (FF:FF:FF:FF:FF:FF, itself registered as a peer): received by *unlimited*
  listeners without peer entries, **no MAC ACK, no retransmission**, and the send callback
  reports success unconditionally. Broadcast cannot be encrypted at the ESP-NOW layer
  [[now-1]](#12-references) [[now-3]](#12-references).
- **Unicast**: MAC-layer ACK plus driver retransmission (retry count undocumented;
  community measurements suggest ~5 tries, failure reported after ~25 ms)
  [[now-4]](#12-references). `ESP_NOW_SEND_SUCCESS` means *the MAC ACK arrived* — nothing
  more. A receiver whose RX buffer overflowed still ACKs, so even "successful" unicast
  needs an application-level acknowledgment where delivery truly matters
  [[now-3]](#12-references).
- Both callbacks run in the high-priority Wi-Fi task: copy out, post to a queue, return
  immediately [[now-1]](#12-references).

The 20-peer table would seem to cap the cluster at 20 nodes. It does not: **broadcast needs
no peer entries**, so a broadcast-first design scales membership past the table. Unicast
lanes are reserved for the small set of relationships that need them (node ↔ coordinator).

### 3.3 Measured performance

Bench numbers from independent sources (two ESP32, short range unless noted):

| Metric | Value | Source |
|---|---|---|
| Default PHY rate | 1 Mbps (802.11b) | [[now-1]](#12-references) |
| Throughput @ 1 Mbps | ~90–100 kB/s app-level; Espressif quotes ~214 kbps open-air | [[perf-1]](#12-references) [[now-2]](#12-references) |
| Throughput @ 54 Mbps / MCS4 | ~780–960 kB/s broadcast | [[perf-1]](#12-references) |
| One-way latency, small frame | median 5.6 ms @ 12 B (default rate, incl. software path) | [[perf-2]](#12-references) |
| Broadcast fan-out | ~800 µs on-air fixed cost; **inter-receiver spread only ~80 µs** | [[perf-3]](#12-references) |
| Sender-side software jitter | up to ~5 ms (CSMA + scheduling) | [[perf-3]](#12-references) |
| Range, PCB antenna, open field | <20 ms latency to 300 m; LR mode usable to ~450–900 m | [[perf-4]](#12-references) |

Airtime arithmetic at the default 1 Mbps *(estimate from sourced constants — 192 µs PLCP
preamble, 39 B overhead)*: a full 250 B broadcast occupies ≈ 2.5–2.9 ms of air including
DIFS and mean backoff → the channel saturates near ~345 such frames/s; a 32 B frame costs
≈ 1.1 ms → ~890 frames/s. These ceilings, times a safety factor, drive the whole traffic
design in §6.3. Cross-check: measured 404 broadcast frames/s at 250 B matches the model
[[perf-1]](#12-references).

Rate control: per-peer since IDF 5.2 (`esp_now_set_peer_rate_config`). ESP8266 has no rate
API and speaks 1 Mbps only — another reason the 8266-audible traffic class stays at
defaults. LR (long-range) mode is ESP32-only, ~4× range at 250/500 kbps, invisible to
ESP8266; a range tool, not a cluster fabric [[now-2]](#12-references) [[perf-4]](#12-references).

### 3.4 Channel discipline

All nodes must share one primary channel. If any node also associates to a Wi-Fi AP as a
station, its radio is dragged to the AP's channel — and APs move channels on their own
schedule, silently severing ESP-NOW links. This is the classic ESP-NOW failure mode
[[now-5]](#12-references). DBrain's rules:

1. Cluster nodes never associate to an AP. The channel is a cluster constant (default: a
   fixed channel 1–11, configurable at provisioning).
2. The one exception is a *gateway* node bridging to a PC (§10); if it must use STA mode,
   it pins the cluster channel to the AP's channel and re-checks after every reconnect.
3. `WIFI_PS_NONE` everywhere: modem sleep adds multi-ms latency and ruins RX timestamp
   precision (§7.2) [[now-1]](#12-references) [[sync-1]](#12-references).

### 3.5 ESP8266 interop contract

ESP8266 (NonOS SDK `espnow.h`) differs enough to deserve an explicit contract: role-based
API (roles are 8266-side bookkeeping only; ESP32 has none), no RX-info struct — **no
hardware RX timestamp and no per-frame RSSI**, callbacks in SDK context, ~200 kbps
practical ceiling, and it drops frames when a fast ESP32 floods it [[now-6]](#12-references)
[[perf-5]](#12-references). Interop with ESP32 is reliable when: same channel, 1 Mbps
8266-audible traffic, ≤ 250 B payloads, matching keys if encryption is used.

DBrain therefore treats ESP8266 as a **leaf tier** (§4.2): full cluster member, smaller
shard, wider timing guard bands, never the coordinator.

---

## 4. Hardware survey: the cluster zoo

### 4.1 Variant matrix

Verified against Espressif product pages (Wikipedia's variant tables contain errors —
trust the vendor pages):

| Chip | ESP-NOW | CPU | SRAM | FPU | Cluster role |
|---|---|---|---|---|---|
| ESP8266 | v1 only | 1× L106 @ 80/160 MHz | ~160 KB (~40–50 KB free heap) | no | leaf |
| ESP32 | yes | 2× LX6 @ 240 MHz | 520 KB | single-prec. | workhorse; + PSRAM: heavyweight |
| ESP32-S2 | yes | 1× LX7 @ 240 MHz | 320 KB | **no** | mid |
| ESP32-S3 | yes | 2× LX7 @ 240 MHz | 512 KB | single-prec. + SIMD | workhorse; + PSRAM: heavyweight |
| ESP32-C2 | yes (2/4 enc. peers) | 1× RISC-V @ 120 MHz | 272 KB | no | leaf/mid |
| ESP32-C3 | yes | 1× RISC-V @ 160 MHz | 400 KB | no | mid |
| ESP32-C5 | yes (dual-band Wi-Fi 6) | 1× RISC-V @ 240 MHz | 384 KB | no | mid |
| ESP32-C6 | yes (Wi-Fi 6, 2.4 GHz) | 1× RISC-V @ 160 MHz + LP core | 512 KB | no | mid |
| ESP32-H2 | **no — no Wi-Fi radio** (BLE + 802.15.4 only) | — | — | — | **cannot participate** |
| ESP32-P4 | **no radio** (companion-chip design) | 2× RISC-V @ 400 MHz | 768 KB | yes + AI ext. | only behind a C/S-series radio companion |

Sources: [[hw-1]](#12-references) [[hw-2]](#12-references). Two honest exclusions fall out
of physics, not software: the H2 has no Wi-Fi radio, and the P4 has no radio at all. "Works
with every variant" in DBrain means *every ESP-NOW-capable variant*, which is every current
Wi-Fi-equipped member of the family.

Note the FPU column: **most of the family has no FPU** (including, surprisingly, the S2).
On a no-FPU chip a software float add costs ~100 cycles and a cosine ~2,400, versus 25 and
121 with hardware float [[hw-3]](#12-references). This single row of the table dictates the
arithmetic format of the whole simulator (§5.3).

### 4.2 Capability tiers

Nodes advertise raw capability numbers (chip ID, cores, MHz, free heap, PSRAM bytes, FPU
flag) in their membership beacons (§8.1); the placement logic (§8.3) consumes the numbers
directly. Tiers are just human-readable shorthand:

- **Leaf** (ESP8266, C2): ~100–500 neurons, sensory/motor endpoints, wide timing guards.
- **Mid** (S2, C3, C5, C6): ~500–1,500 neurons.
- **Workhorse** (ESP32, S3): ~1,000–2,000 neurons, eligible coordinator.
- **Heavyweight** (ESP32/S3 + PSRAM): the same neurons but 10⁵–10⁶ synapses of fan-in.

---

## 5. Neuron, synapse, and simulation model

### 5.1 Choosing the neuron model

Izhikevich's canonical comparison of spiking models — cost in operations to simulate 1 ms,
versus how many of ~20 known biological firing behaviors each reproduces
[[snn-1]](#12-references):

| Model | State vars | Ops per ms | Behaviors |
|---|---|---|---|
| Leaky integrate-and-fire | 1 | ~5 | 3 ("one of the worst models to use") |
| Quadratic I&F | 1 | 7 | 6 |
| **Izhikevich (2003)** | **2** | **13** | **all 20** |
| FitzHugh–Nagumo | 2 | 72 | ~11 |
| Hodgkin–Huxley | 4 | ~1,200 | all 20 (biophysical) |

The choice makes itself: **Izhikevich** — two state variables, 13 operations, and the full
behavioral repertoire (regular spiking, bursting, chattering, resonating…) selected per
population by four parameters (a, b, c, d). A 1,000-neuron Izhikevich network ran real-time
on a 1 GHz desktop in interpreted MATLAB in 2004 [[snn-1]](#12-references); a 240 MHz ESP32
core doing 13 integer ops × 1,000 neurons per tick spends well under a millisecond
*(estimate)*. LIF ships as a cheaper option per population, since it is a near-subset.

### 5.2 The simulation loop: hybrid, SpiNNaker-style

Time-driven neurons, event-driven synapses — the structure every serious real-time SNN
engine converged on [[spin-2]](#12-references) [[snn-2]](#12-references):

1. Every tick, integrate all local neuron states (fixed cost, cache-friendly array sweep).
2. Synaptic work happens **only when a spike arrives**: look up the source neuron's
   synaptic row, walk it, add each weight to the target neuron's input accumulator at
   `now + delay`.
3. Spikes raised locally are queued for the tick's TX window (§6.2).

With biological firing rates of 0.1–10 Hz (lognormal, long tail) [[bio-4]](#12-references),
event-driven synapses cost 100–10,000× less than touching every synapse every tick — the
sparsity of the brain is the only reason a radio-linked cluster is feasible at all.

Input accumulation uses a ring of per-tick buckets indexed by delay (§6.4 explains why
delays also buy network-jitter tolerance).

### 5.3 Arithmetic: fixed-point everywhere

Two forcing facts: most of the family lacks an FPU (§4.1), and a heterogeneous cluster
must produce *identical* results regardless of which chip a neuron landed on — placement
(§8.3) must not change the simulation. Both point the same way:

- **All neuron and synapse math is integer fixed-point.** Core format **s16.15** (the
  ISO `accum` format), the exact format SpiNNaker's production stack uses for Izhikevich
  and LIF, with published accuracy analyses [[snn-3]](#12-references)
  [[snn-4]](#12-references). Weights stored as 16-bit.
- Per-tick decay factors (e.g. e^(−dt/τ)) are precomputed constants — no transcendental
  functions at runtime.
- Bit-identical math across chips also makes any node's result reproducible on the host
  simulator (§10), which is the debugging story.

FPU-equipped chips simply execute the same integer code faster.

### 5.4 Memory: the synapse budget

Synapses, not neurons, are the memory story. The packed synapse word is borrowed from
SpiNNaker: **32 bits = 16-bit weight + 4-bit delay + 8-bit local target index + type/pad**
[[snn-3]](#12-references) — grown to 64 bits per synapse only where a population needs
plasticity state (§5.5). Neuron state is 8 B (v, u in s16.15) plus shared per-population
parameters.

Budgets per node *(estimates from sourced RAM figures [[hw-1]](#12-references)
[[snn-5]](#12-references))*:

| Node | Usable RAM for sim | Neurons | Synapses |
|---|---|---|---|
| ESP32 / S3, internal RAM only | ~250 KB | 1,000 (8 KB) | ~60,000 @ 4 B |
| ESP32 / S3 + 4 MB PSRAM | +4 MB | 1,000–2,000 | **~10⁶** (~500–1,000 fan-in per neuron — biologically respectable) |
| ESP32-C3 | ~150 KB | ~750 | ~30,000 |
| ESP8266 | ~40 KB | ~200–500 | ~8,000 |

The PSRAM pattern mirrors SpiNNaker's SDRAM split exactly: neuron state in internal RAM
(touched every tick), synaptic rows in PSRAM (streamed sequentially on spike arrival — the
one access pattern slow external RAM is good at) [[spin-2]](#12-references). Published
cross-check: a full-featured SNN engine on an RP2350 with 8 MB PSRAM ran 1,200 neurons,
186 in hard real time [[snn-5]](#12-references) — same silicon class, same order of
magnitude as the table.

### 5.5 Plasticity (deferred, designed-for)

Trace-based STDP is O(1) per spike [[snn-6]](#12-references); SpiNNaker's deferred
event-driven variant reached ~500 K plastic synaptic events/s on a 200 MHz ARM968 — an
ESP32 core is faster [[snn-7]](#12-references). Costs: roughly 2× synapse memory on plastic
populations and ~half the sustainable spike throughput. Because source spikes arrive at the
node that owns the synapse and the postsynaptic neuron is local, **distribution adds no new
problem for STDP** — the SpiNNaker scheme transfers unchanged. Ships as a later phase
(§11.3), but the 64-bit plastic synapse layout is reserved now.

---

## 6. Spike transport design

### 6.1 AER over broadcast: everyone hears everything

In neuromorphic engineering, spikes cross chip boundaries as **Address Event
Representation**: a packet carries only *which neuron fired* (and implicitly when); it
never carries membrane state [[aer-1]](#12-references). A spike is ~1 bit of information;
a membrane potential is a continuous variable needing constant sampling. AER is why
inter-chip links survive at all.

SpiNNaker delivers AER packets through hardware multicast routers with compiled routing
tables. DBrain gets the same semantics free from the physics of radio: **an ESP-NOW
broadcast is hardware multicast**. Every spike bundle is broadcast once; every node
receives it; each node checks the source ID against its local **subscription table** (a
sorted key → synaptic-row index, the software equivalent of SpiNNaker's ternary CAM) and
either processes or discards in O(log n). No routing tables, no forwarding, no per-peer
state, no 20-peer limit. The cost — every node's radio hears all traffic — is unavoidable
anyway on a shared channel; the win is that a spike consumed by seven nodes costs one
frame, not seven.

Single-hop broadcast reach is the defining topology assumption: DBrain v1 targets a
cluster within one radio cell (a desk, a room — hundreds of meters line-of-sight is fine,
§3.3). Multi-hop flooding meshes exist but collapse under dense traffic
[[prior-6]](#12-references); out of scope until proven necessary.

### 6.2 The spike bundle

One frame type carries the simulation. Capped at 250 B so every tier hears it (§3.1):

```
SpikeBundle (broadcast, ≤ 250 B payload)
  u16  magic + version
  u16  source node id          (compact id assigned at join, §8.1)
  u32  tick number             (biological time, in ticks)
  u16  bundle sequence number  (per-node monotonic; receivers count gaps)
  u8   event count
  u8   flags
  u16  events[count]           (node-local neuron index)
```

12 B header + 2 B per event → **119 spike events per frame**. Global neuron identity is
(node id, local index) — 32 bits total, matching classic AER practice
[[aer-2]](#12-references). A node accumulates its tick's spikes and transmits them as one
bundle (two if a burst overflows 119); a quiet tick transmits nothing except a heartbeat
cadence (§8.1).

Sequence numbers make loss *visible* (a gap is a counted drop, surfaced in telemetry §9.3)
without making it *recovered* — recovery is deliberately absent (§6.4).

### 6.3 Airtime budget: the honest bottleneck

The channel is the cluster's backplane, and it is narrow. Safe capacity at ~30 % channel
load (broadcast collisions are unrecoverable; light loading is the collision-avoidance
strategy) *(estimate on sourced airtime constants, §3.3)*:

| PHY rate | Safe bundles/s (cluster-wide) | Safe spike events/s |
|---|---|---|
| 1 Mbps (default; ESP8266-compatible) | ~100 | ~12,000 |
| 24–54 Mbps (ESP32-only cluster) | ~600–1,150 | ~70,000–137,000 |

Against demand: 10 nodes × 1,000 neurons × 3 Hz mean ≈ 30,000 events/s — comfortably
inside the ESP32-only budget, 2.5× over the 1 Mbps budget. Consequences, in order:

1. **Batching is mandatory.** 119 events amortize one frame's ~2.9 ms airtime; per-spike
   frames would saturate the channel at ~300 spikes/s total.
2. **Mixed clusters run slower than real time.** The tick's wall-clock length is a cluster
   parameter (§7.1); a 1 Mbps cluster runs at, say, 5–10 ms per biological millisecond.
   An ESP32-only cluster at a high PHY rate can target true real time. This is precisely
   SpiNNaker's own history — the cortical microcircuit ran at 20× slowdown before later
   optimization reached real time [[spin-5]](#12-references).
3. **Bursts, not means, size the system.** Synchronous population bursts (gamma-band
   oscillations) are the design load; §7.3's TDMA slotting keeps burst-time collisions off
   the air, and §6.4's delay slack absorbs the queueing.
4. **Placement minimizes radio traffic** (§8.3): neurons that wire together get placed
   together, so most synapses stay on-board and never touch the air.

### 6.4 Drop, don't retry — and let biology absorb the jitter

Spike bundles are fire-and-forget. No ACKs, no retransmission, ever, on the spike plane.
This is a considered position, imported from SpiNNaker's congestion philosophy
[[spin-3]](#12-references): a retransmitted spike is a *late* spike, late spikes are wrong
spikes (or stalled ticks), and neural computation degrades gracefully under a fraction of
a percent of spike loss where it degrades catastrophically under lost real-time behavior.
Loss is counted per link (sequence gaps) and reported, not repaired. Control-plane traffic
(join, assignment, migration — §8) takes the opposite contract: app-level ACK + retry over
unicast, following the reliability header design of Espressif's own esp-now component
[[now-7]](#12-references).

The elegant trick — TrueNorth's delivery-tick field and SpiNNaker's practice, adopted
wholesale [[nm-2]](#12-references): **synaptic delay absorbs network latency.** Biological
synapses have 1–15 ms delays (our 4-bit delay field). A bundle stamped tick *T* arriving
during *T+1* still applies its weights at *T + delay* for every synapse with delay ≥ 2 —
on time, biologically exact. As long as one-way transport stays under the minimum synaptic
delay in wall-clock terms, radio jitter is *invisible to the simulation*. The compiler
(§8.3) enforces a minimum delay of 2 ticks on all cross-node projections; on-board synapses
may use delay 1.

---

## 7. Time: synchronization and the tick

### 7.1 The tick model

Logical time advances in **ticks**; one tick = 1 ms of biological time. The wall-clock
duration of a tick (the *slowdown factor*) is a cluster constant chosen at network compile
time from the airtime budget: 1× for small/fast clusters, 5–10× for large or
ESP8266-inclusive ones (§6.3). Within each tick every node runs the same phase schedule:

```
|-- integrate neurons --|-- TX window (TDMA slots) --|-- guard --|
tick N                                                            tick N+1
```

There is **no per-tick barrier** (SpiNNaker's lesson: barriers make the slowest node the
cluster clock, and a lost barrier message stalls the world [[spin-2]](#12-references)). A
node that overruns processes its backlog during quieter ticks; delay slack (§6.4) hides
transient lag. The coordinator's tick beacon (§7.2) doubles as a rate anchor — a node that
drifts a full tick behind resyncs forward and logs the event (equivalent to dropped local
compute, counted like any other loss).

For experiments needing hard reproducibility, a **paced mode** exists: the coordinator
advances the tick counter only after DONE reports from all members (or a deadline), at the
cost of running as slow as the slowest node. Default is free-running.

### 7.2 Clock synchronization

The measured physics dictates the scheme (§3.3): a broadcast reaches all receivers within
~80 µs of each other, while the *sender's* transmit instant wobbles by up to 5 ms. So sync
must key off **reception**, never transmission — the insight of Reference Broadcast
Synchronization [[sync-2]](#12-references). ESP-IDF ≥ 5.1 provides exactly the needed
primitive: the RX callback's `rx_ctrl.timestamp`, a microsecond PHY-level stamp of the
arrival instant, precise while sleep is disabled [[sync-1]](#12-references). (The Wi-Fi TSF
timer is unusable — it never runs without an AP association [[sync-3]](#12-references).)

Design (FTSP-style flooding root, adapted to hardware RX stamps
[[sync-4]](#12-references)):

- The coordinator broadcasts a **tick beacon** every beacon interval (default 1 s):
  `(epoch, tick number, coordinator local time at previous beacon's TX completion)`.
- Receivers pair each beacon's content with their own `rx_ctrl.timestamp`, maintain an
  8-point linear regression of (coordinator time vs local time), and slew — never step —
  their tick phase. Regression kills both offset and crystal-skew error.
- Crystal budget: Espressif requires ±10 ppm crystals [[sync-5]](#12-references); two nodes
  drift apart ≤ 20 µs/s worst case, so even the raw drift between 1 s beacons (≤ 20 µs) is
  far inside a 1 ms tick, and skew regression reduces residual rate error to ~1 ppm class
  [[sync-4]](#12-references) *(interval math: estimate)*.
- **Accuracy target: < 100 µs cluster-wide, honestly stated.** Published µs-class WSN
  results (FTSP: 1.5 µs/hop) relied on MAC-layer *TX* timestamping, which ESP-IDF does not
  expose; the ~80 µs receiver spread is the realistic floor here, and the best documented
  ESP-NOW sync claims sit at ±10–50 µs without rigorous measurement
  [[sync-6]](#12-references). 100 µs is 10 % of a tick — ample for slotting, and the
  simulation itself only needs tick-level agreement.
- ESP8266 nodes lack `rx_ctrl` and timestamp in software at callback entry: ~ms-level
  sync. Their TDMA slots simply get wider guard bands (they are leaf tier with modest
  traffic anyway).

Sync cost: one ~40 B beacon/s ≈ 0.1 % airtime *(estimate)*.

### 7.3 TDMA slotting: sync pays for itself

Once nodes share time to < 100 µs, the TX window of each tick is divided into **assigned
slots** (assignment rides on the membership table, §8.1). Each node transmits its bundle
in its own slot; CSMA remains as a safety net, not the mechanism. This converts the
broadcast channel's worst enemy — synchronized burst collisions, plus the hidden-node
problem that broadcast cannot ACK away [[now-8]](#12-references) — into scheduled,
collision-free airtime. Sync enables slotting; slotting protects the beacons that maintain
sync. The loop closes.

---

## 8. Self-organization: discovery, election, placement

### 8.1 Membership: hear the heartbeats

All membership state derives from broadcast **heartbeats** (1 Hz cadence, jittered ±10 %
to avoid phase-locking): node MAC, compact node id (assigned at join), capability record
(chip id, cores, MHz, free heap, PSRAM, FPU, tier), current epoch, shard version, and
health counters. On a broadcast channel every node overhears every heartbeat, so passive
listening replaces most of a gossip protocol's active machinery; a membership entry is
~24 B, so even a 100-node table costs ~2.4 KB — ESP8266-fine *(estimate; protocol shape
after SWIM [[dist-1]](#12-references))*.

Failure detection: 3 missed heartbeats → *suspect*; SWIM-style indirect probe (ask two
other nodes to ping the suspect over unicast — cheap insurance against asymmetric radio
links [[dist-1]](#12-references)); confirmed-dead → coordinator triggers reassignment
(§8.4). A brand-new board broadcasts JOIN beacons, cycling channels until it hears the
cluster, then receives its node id and (if a spare pool or rebalance allows) a shard from
the coordinator — the established ESP-NOW auto-pairing pattern [[dist-2]](#12-references).

### 8.2 Coordinator election: boring on purpose

The coordinator is *not a master*: the spike plane is fully peer-to-peer and keeps running
— degraded but alive — with no coordinator at all. The coordinator owns only slow-path
duties: tick/sync beacons, join admission, placement changes, epoch numbering. That
narrow role permits the simplest election that works, the same one FTSP ships
[[sync-4]](#12-references):

- Election key = **(capability score ‖ MAC)**, highest wins — capability prefix so a
  PSRAM-equipped S3 beats a C3, MAC suffix so ties are impossible. Leaf-tier nodes are
  ineligible.
- A node hearing no coordinator beacon for a timeout self-promotes and starts beaconing
  with a bumped **epoch number**; any node hearing a higher key (or higher epoch) yields
  immediately. Hysteresis on the timeout prevents flapping under marginal links.
- Partitions therefore heal deterministically on merge: higher epoch wins, then higher
  key. Split-brain during a partition costs nothing fatal — two half-clusters each keep
  simulating their shards; spikes across the split are (counted) losses like any others.

Raft-class consensus was evaluated and rejected: it presumes reliable ordered unicast to
named peers and buys a replicated log nobody needs here — a distinguished beacon source
plus an epoch counter is the entire requirement [[dist-3]](#12-references).

### 8.3 Placement: the network compiler

"Which neuron lives where" is a graph-partitioning problem solved *before* the simulation
starts, host-side (§10), then adjusted at runtime by the coordinator (§8.4).

The compiler ingests a population/projection description (PyNN-style semantics: named
populations with model parameters; projections with connectivity rules, weight/delay
distributions [[tool-1]](#12-references)) and produces per-node artifacts: neuron parameter
blocks, packed synaptic rows, subscription tables, and the cluster manifest (channel, PHY
rate, tick slowdown, slot map).

Partitioning objective — directly transplanted from the SNN mapping literature
[[part-1]](#12-references) [[part-2]](#12-references):

- **Minimize expected radio traffic**, not edge count: the cost of assigning source
  neuron *s* off-node from a subscriber node is (firing rate of *s*) × 1 — counted **once
  per (source, subscribing node)**, not once per synapse, because broadcast delivers a
  bundle to all subscribers for one frame (§6.1). This multicast-aware objective is a
  hypergraph partitioning problem; SpiNeMap demonstrated 45 % energy reductions by using
  *measured* spike rates rather than static topology — DBrain adopts the same refinement
  loop (run, measure per-population rates via telemetry, re-place).
- **Balance tick compute time, not neuron count** (BSP's slowest-node rule
  [[dist-4]](#12-references)): each node's shard is sized against its advertised
  capability so all tiers finish integration inside the tick.
- **Respect memory**: synapse bytes ≤ node budget (§5.4).
- Constraint from §6.4: cross-node projections get delay ≥ 2 ticks.

Biological structure helps: cortical connectivity falls off with distance, so
spatially-informed partitions keep the majority of synapses on-board — but the literature
warns the non-local tail is fat [[part-3]](#12-references), which is why the transport
budget (§6.3) is engineered rather than wished away.

**Compilerless mode** (self-assembly, for the pure out-of-the-box experience): with no
host in the loop, the coordinator broadcasts a generative recipe — (topology family,
parameters, RNG seed). Each node *derives* its own population and connectivity
deterministically from (recipe, node id, capability), e.g. distance-dependent random
recurrent networks over a virtual coordinate per node. No tables are shipped anywhere;
the network exists as a pure function of the recipe. Cheap, instant, and surprisingly
close to how biology wires by rule rather than by blueprint.

### 8.4 Runtime reassignment

Membership changes trigger the coordinator, at epoch boundaries only (fine-grained work
stealing is uneconomical when moving one shard means shipping kilobytes through 250 B
frames [[dist-5]](#12-references)):

- **Node death**: its shard's neurons are respawned from the manifest (initial state, not
  checkpointed state) on nodes with headroom, or parked if none exists. Losing transient
  membrane state is accepted and logged — the biological analogy is honest, and
  checkpointing every shard continuously through the radio budget is not (§11.2 keeps
  optional checkpointing open).
- **Node join**: coordinator either assigns parked/spare shards or schedules a rebalance:
  STOP-tick fence → state transfer over reliable unicast lane → subscription-table update
  broadcast → RESUME. Consistent-hashing-style minimal movement is the guiding principle
  (move only what must move [[dist-6]](#12-references)), though placement quality (§8.3)
  outranks movement minimality.
- Every reassignment increments the **shard version** in heartbeats, so stale bundles
  from pre-migration senders are detected and discarded by version check.

---

## 9. Reliability, security, operations

### 9.1 Two planes, two contracts

| | Spike plane | Control plane |
|---|---|---|
| Transport | broadcast bundles | unicast, app-ACK + retry (≤ 31, esp-now-component style [[now-7]](#12-references)) |
| Loss policy | drop + count (§6.4) | reliable delivery |
| Volume | ~all airtime | ~nothing |
| Members | all nodes | node ↔ coordinator, node ↔ gateway |

### 9.2 Security posture

Stated plainly: **v1 is an open lab instrument, not a secure system.** ESP-NOW broadcast
frames cannot be encrypted at the protocol layer [[now-3]](#12-references), and per-peer
encryption caps at 17 peers — both incompatible with a broadcast-first cluster. The v1
threat model is "someone's other ESP-NOW project on the same channel," handled by the
cluster id in every header. The upgrade path when needed: application-layer AES-CCM with
a cluster-wide key over broadcast (replay-protected by tick/sequence fields already in the
header), keyed at provisioning via ECDH over the unicast lane — the same construction as
Espressif's esp-now security component [[now-7]](#12-references). Deferred, not precluded.

### 9.3 Observability

Every node maintains counters (bundles TX/RX, sequence-gap drops per source, late bundles,
tick overruns, heap low-water, sync residual) surfaced three ways: piggybacked summaries
in heartbeats, a telemetry query on the control plane, and the gateway's live stream
(§10). SpiNNaker's discipline applies: **a dropped spike is not an error; an uncounted
dropped spike is** [[spin-3]](#12-references). Remote log capture over ESP-NOW (the
esp-now debug component pattern [[now-7]](#12-references)) covers nodes without USB
attached.

### 9.4 Firmware lifecycle

One firmware image for all tiers per architecture (Xtensa/RISC-V/8266 builds of one
codebase); node behavior is data-driven by the manifest. Updates over the air, no router
required: Espressif's esp-now OTA component subpackages an image over broadcast with
per-device gap-filling and resume ("50 devices in 3 minutes," vendor figure
[[now-7]](#12-references)) for IDF-based tiers; ESP8266 falls back to its own path
(gateway-served HTTP OTA in SoftAP mode). Rollback on failed boot via the standard
dual-partition scheme.

---

## 10. Host tooling and visualization

The cluster is self-sufficient (compilerless mode), but the full experience needs a PC in
the loop, connected through a **gateway node** (any workhorse-tier board, USB serial to
the host; it is a normal cluster member that additionally mirrors traffic).

- **`dbrainc` — the network compiler** (Python): PyNN-flavored population/projection
  definitions in, per-node artifacts and cluster manifest out (§8.3). PyNN semantics are
  the deliberate choice — it is the API the actual neuromorphic machines expose, and its
  concepts (populations, projections, connectivity rules) are exactly the right
  granularity for placement [[tool-1]](#12-references). A reference software simulator
  (same fixed-point math, bit-identical) runs any network host-side for verification
  before deployment.
- **Reference-model parity is the debugging story**: same s16.15 arithmetic on host and
  device means a diverging node is diagnosable by replay.
- **Dashboard** (browser, fed by the gateway over websocket): live spike raster per
  population, per-node activity heatmap laid out on the cluster map, tick/airtime/drop
  telemetry, and the membership table. Prior art to emulate: NEST Desktop's
  network-editor-plus-raster layout and Nengo GUI's live-manipulation-while-running
  pattern [[tool-2]](#12-references). Injecting stimuli from the dashboard (sliders →
  input populations) makes the cluster a playable instrument — the demo that keeps a
  project alive (§2.3).

---

## 11. Risks, open questions, roadmap

### 11.1 Top risks, ranked

1. **Airtime is the wall** (§6.3). Mitigations are in-design (batching, TDMA, placement,
   slowdown factor); the residual risk is bursty workloads exceeding the 30 % budget.
   Falsifiable early: Phase 0 measures the real saturation curve.
2. **Sync floor on real boards.** The <100 µs target rests on `rx_ctrl.timestamp`
   behavior across variants; no published measurement rigorously validates tens-of-µs
   sync over ESP-NOW [[sync-6]](#12-references). Phase 2 measures it with GPIO + logic
   analyzer before anything depends on it.
3. **ESP8266 drag.** The 250 B / 1 Mbps compatibility contract taxes the whole cluster's
   airtime when any 8266-audible traffic exists. Mitigation: mixed clusters accept the
   slowdown factor; pure-ESP32 clusters negotiate a high PHY rate at formation.
4. **Wi-Fi neighborhood interference.** A busy 2.4 GHz environment eats the airtime
   budget unpredictably. Channel selection at provisioning + telemetry makes it visible;
   it cannot be engineered away.
5. **Scope creep** — the documented killer of every neighbor project (§2.3). The roadmap
   below ships a demonstrable artifact per phase.

### 11.2 Open questions (deliberately unresolved)

- Optional shard checkpointing to coordinator/host for long-running experiments — worth
  its airtime?
- ESP-NOW v2 frames (1470 B) as a negotiated fast path for ESP32-only clusters on
  IDF ≥ 5.4 — real-world gain vs added protocol complexity?
- Second radio cell bridging (dual-homed gateway nodes) if clusters outgrow one cell.
- P4-behind-companion as a heavyweight compute tier — is the companion-link latency
  acceptable?

### 11.3 Roadmap

Each phase ends with something that runs and a number that was measured.

- **Phase 0 — Transport truth.** Two, then five boards; measure throughput, latency
  distribution, loss vs rate/payload/distance, broadcast fan-out spread. Deliverable: a
  bench report that replaces §3.3's borrowed numbers with ours.
- **Phase 1 — Two boards, one network.** Hardcoded shards, spike bundles, subscription
  tables, drop counters. Deliverable: a synfire chain spiking across the air, raster
  dumped over serial.
- **Phase 2 — Time.** Tick beacons, RX-timestamp regression sync, TDMA slots; measure
  sync residual with logic analyzer. Deliverable: measured cluster sync accuracy.
- **Phase 3 — Self-organization.** Heartbeats, join flow, election, failure detection,
  compilerless generative mode. Deliverable: boards added/removed live while the raster
  keeps running.
- **Phase 4 — The compiler.** `dbrainc`, manifest deployment, reference simulator parity.
  Deliverable: a PyNN-style script running on N boards, verified against host replay.
- **Phase 5 — The instrument.** Gateway + dashboard, live stimuli, telemetry. Deliverable:
  the playable demo.
- **Phase 6 — Depth.** STDP populations, rebalancing, OTA, mixed-tier reference cluster.
  Deliverable: a learning network on heterogeneous hardware surviving node churn.

---

## 12. References

**Biology / scale**
- [bio-1] Herculano-Houzel, "The human brain in numbers," *Front. Hum. Neurosci.* 2009. — 86 B neurons.
- [bio-2] Dorkenwald et al., FlyWire whole-brain connectome, *Nature* 2024. https://www.nature.com/articles/s41586-024-07558-y — 139,255 neurons / 54.5 M synapses.
- [bio-3] White et al. 1986; Cook et al., *Nature* 2019 — *C. elegans* connectome. https://wormwiring.org/
- [bio-4] Buzsáki & Mizuseki, "The log-dynamic brain," *Nat. Rev. Neurosci.* 2014 — lognormal firing rates ~0.1–10 Hz.

**SpiNNaker**
- [spin-1] Furber et al., "Overview of the SpiNNaker System Architecture," *IEEE Trans. Computers* 2013.
- [spin-2] Rhodes et al., "sPyNNaker: A Software Package for Running PyNN Simulations on SpiNNaker," *Front. Neurosci.* 2018. https://www.frontiersin.org/journals/neuroscience/articles/10.3389/fnins.2018.00816/full
- [spin-3] Rowley et al., "SpiNNTools: The Execution Engine for the SpiNNaker Platform," 2018. https://arxiv.org/pdf/1810.06835 — drop/reinjection policy.
- [spin-4] Mayr, Höppner, Furber, "SpiNNaker 2," 2019. https://arxiv.org/abs/1911.02385
- [spin-5] van Albada et al., *Front. Neurosci.* 2018 (cortical microcircuit on SpiNNaker); Rhodes et al. 2019 real-time follow-up. https://arxiv.org/abs/1909.08665

**Other neuromorphic machines**
- [nm-1] Davies et al., "Loihi: A Neuromorphic Manycore Processor with On-Chip Learning," *IEEE Micro* 2018; WikiChip: https://en.wikichip.org/wiki/intel/loihi
- [nm-2] Akopyan et al., "TrueNorth," *IEEE TCAD* 2015 — mesh routing, delivery-tick field.
- [nm-3] Pehle et al., "The BrainScaleS-2 accelerated neuromorphic system," *Front. Neurosci.* 2022. https://www.frontiersin.org/journals/neuroscience/articles/10.3389/fnins.2022.795876/full

**Prior projects**
- [prior-1] Spikeling: Baden lab, *PLOS Biology* 2018. https://github.com/OpenSourceNeuro/Spikeling / V2: https://github.com/MaxZimmer/Spikeling-V2
- [prior-2] NeuroBytes. https://hackaday.io/project/3339-neurobytes
- [prior-3] Lu.i neuron PCB. https://hackaday.io/project/190754-lui-educational-neuron-pcb
- [prior-4] Broccoli ESP32 cluster. https://github.com/Wei1234c/Broccoli ; Hackaday 2018-04-17 coverage.
- [prior-5] "Decentralized Distributed Sequential Neural Networks Inference on Low-Power Microcontrollers in WSNs," *Sensors* 25(15):4595, 2025. https://doi.org/10.3390/s25154595
- [prior-6] EspNowFloodingMesh. https://github.com/arttupii/EspNowFloodingMesh (maintained fork: leodesigner/espNowFloodingMeshLibrary2)

**ESP-NOW protocol**
- [now-1] ESP-IDF ESP-NOW reference. https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_now.html
- [now-2] Espressif FAQ, ESP-NOW section. https://docs.espressif.com/projects/esp-faq/en/latest/application-solution/esp-now.html
- [now-3] MicroPython espnow module docs (broadcast/ACK semantics, buffer-overflow ACK caveat). https://docs.micropython.org/en/latest/library/espnow.html
- [now-4] Retry behavior: https://github.com/espressif/esp-idf/issues/9383 ; https://github.com/orgs/micropython/discussions/11757
- [now-5] Channel-drift threads: https://www.esp32.com/viewtopic.php?t=14542 ; https://circuitlabs.net/esp-now-with-wifi-coexistence/
- [now-6] ESP8266 NonOS SDK espnow.h + API reference PDF (roles, key-of-keys, peer limits). https://github.com/espressif/ESP8266_NONOS_SDK/blob/master/include/espnow.h
- [now-7] Espressif esp-now component (reliability header, provisioning, OTA, security, debug). https://github.com/espressif/esp-now
- [now-8] Hidden-node problem overview. https://www.sciencedirect.com/topics/computer-science/hidden-node-problem

**Measured performance**
- [perf-1] NTU throughput benchmarks (rates × unicast/broadcast). https://blogs.ntu.edu.sg/ps9888-2020-g16/2020/07/11/esp-now-data-transfer/
- [perf-2] Electric UI latency comparison (logic-analyzer instrumented). https://electricui.com/blog/latency-comparison
- [perf-3] Broadcast fan-out timing measurements. https://forum.arduino.cc/t/esp-now-broadcast-transmisison-latency/1347159
- [perf-4] Espressif dev blog, ESP-NOW outdoors + LR mode field test. https://developer.espressif.com/blog/esp-now-for-outdoor-applications/
- [perf-5] QuickESPNow measurements (ESP32 vs ESP8266 ceilings). https://github.com/gmag11/QuickESPNow

**Hardware**
- [hw-1] Espressif product pages. https://www.espressif.com/en/products/socs (per-chip: esp32-c5, esp32-c6, esp32-h2, esp32-p4, …)
- [hw-2] ESP-IDF memory-types / RAM-usage guides. https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/memory-types.html
- [hw-3] Espressif developer blog, "Which ESP32 cores have an FPU" (cycle benchmarks), Oct 2025. https://developer.espressif.com/blog/2025/10/cores_with_fpu/

**SNN models & implementation**
- [snn-1] Izhikevich, "Which Model to Use for Cortical Spiking Neurons?", *IEEE Trans. Neural Netw.* 2004. https://www.izhikevich.org/publications/whichmod.pdf
- [snn-2] Brette et al., "Simulation of networks of spiking neurons: a review," *J. Comput. Neurosci.* 2007.
- [snn-3] Rhodes et al. 2018 (synapse word format, s16.15) — see [spin-2].
- [snn-4] Hopkins & Furber, "Accuracy and Efficiency in Fixed-Point Neural ODE Solvers," *Neural Computation* 27, 2015. https://direct.mit.edu/neco/article/27/10/2148/8085
- [snn-5] "Full Feature Spiking Neural Network Simulation on Micro-Controllers for Neuromorphic Applications at the Edge" (RP2350 CARLsim port). https://arxiv.org/abs/2604.16474
- [snn-6] Morrison, Diesmann, Gerstner, "Phenomenological models of synaptic plasticity based on spike timing," *Biol. Cybern.* 2008.
- [snn-7] Diehl & Cook, "Efficient Implementation of STDP Rules on SpiNNaker," IJCNN 2014.

**AER**
- [aer-1] Boahen, "Point-to-point connectivity between neuromorphic chips using address events," *IEEE TCAS-II* 2000.
- [aer-2] Mayr, George, Tambaro, Indiveri, "UDP-based AER communication," 2025. https://arxiv.org/pdf/2501.09128

**Time synchronization**
- [sync-1] `wifi_pkt_rx_ctrl_t` timestamp field semantics (IDF ≥ 5.1) — see [now-1] and IDF Wi-Fi driver types.
- [sync-2] Elson, Girod, Estrin, "Fine-Grained Network Time Synchronization using Reference Broadcasts" (RBS), OSDI 2002.
- [sync-3] TSF-without-AP discussion. https://www.esp32.com/viewtopic.php?t=33611 ; `esp_wifi_get_tsf_time` latency: https://github.com/espressif/esp-idf/issues/7244
- [sync-4] Maróti et al., "The Flooding Time Synchronization Protocol" (FTSP), SenSys 2004. https://www.math.u-szeged.hu/tagok/mmaroti/okt/2009o/ftsp.pdf
- [sync-5] Espressif hardware design guidelines (±10 ppm crystal requirement). https://docs.espressif.com/projects/esp-hardware-design-guidelines/
- [sync-6] ESPNowTimeSync (±10–50 µs claim, NTP-style). https://github.com/jensb1/ESPNowTimeSync ; FTSP-on-ESP-NOW: ACM SIET 2023, https://dl.acm.org/doi/10.1145/3626641.3626683 ; ESPNowMeshClock: https://github.com/Hemisphere-Project/ESPNowMeshClock

**Distributed systems**
- [dist-1] Das, Gupta, Motivala, "SWIM: Scalable Weakly-consistent Infection-style Process Group Membership Protocol," DSN 2002. https://www.cs.cornell.edu/projects/Quicksilver/public_pdfs/SWIM.pdf
- [dist-2] ESP-NOW auto-pairing pattern. https://randomnerdtutorials.com/esp-now-auto-pairing-esp32-esp8266/
- [dist-3] Leader election in IoT survey. https://arxiv.org/abs/1911.00759 ; Raft-on-ESP8266 thesis artifact: https://github.com/kristianmitk/rasp
- [dist-4] Valiant, "A bridging model for parallel computation" (BSP), *CACM* 1990.
- [dist-5] Distributed work-stealing surveys: https://arxiv.org/pdf/2401.04494
- [dist-6] Consistent/rendezvous hashing overviews. https://blog.algomaster.io/p/consistent-hashing-explained

**Partitioning / mapping**
- [part-1] Balaji et al., "Mapping Spiking Neural Networks to Neuromorphic Hardware" (SpiNeMap), *IEEE TVLSI* 2020. https://arxiv.org/abs/1909.01843
- [part-2] Karypis & Kumar, METIS. https://github.com/KarypisLab/METIS
- [part-3] Stepanyants et al., "The fractions of short- and long-range connections in the visual cortex," *PNAS* 2009. https://www.pnas.org/doi/10.1073/pnas.0810390106

**Host tooling**
- [tool-1] PyNN. https://neuralensemble.org/PyNN/ — the simulator-independent SNN API, also the user API of SpiNNaker and BrainScaleS.
- [tool-2] NEST Desktop. https://nest-desktop.github.io/ ; Nengo GUI: https://github.com/nengo/nengo-gui
