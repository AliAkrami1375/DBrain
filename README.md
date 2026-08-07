<div align="center">

<img src="logo.PNG" alt="DBrain" width="220"/>

# DBrain

**A wireless brain, one board at a time.**

Distributed spiking neural network simulation on clusters of ESP32 / ESP8266
microcontrollers, communicating entirely over ESP-NOW.

[English](README.md) · [فارسی](README.fa.md)

![Status](https://img.shields.io/badge/status-research_complete,_implementation_in_progress-blue)
![Platform](https://img.shields.io/badge/platform-ESP32_family_+_ESP8266-orange)
![Transport](https://img.shields.io/badge/transport-ESP--NOW-green)

</div>

---

## What is DBrain?

DBrain turns a pile of cheap Wi-Fi microcontrollers into a single simulated brain.
Every board runs a shard of spiking neurons and synapses; when a neuron fires, the
spike crosses the air as an ESP-NOW broadcast and lands in the synapses of neurons
living on other boards. There is no router, no server, and no wiring between nodes —
power a new board anywhere near the cluster and the brain grows.

The design borrows its core ideas from the large neuromorphic machines —
SpiNNaker's source-addressed multicast spikes, Loihi's logical timesteps,
TrueNorth's quantized global tick — and rebuilds them at three-dollars-per-node
scale, over radio instead of a backplane.

## Is it actually possible?

Yes — and we did the homework before writing a line of firmware. A full research
phase pinned down every hard constraint from primary sources and real-world
measurements. The complete study, with references for every number, lives in this
repository's design document (research phase deliverable; being published alongside
the code as implementation lands). Highlights:

| Question | Finding |
|---|---|
| Has anyone done this before? | **No.** Extensive search found no project — hobby or academic — exchanging spikes between microcontrollers over radio. The closest relatives are single-chip neuron instruments and wired neuron boards. |
| Can one board simulate neurons in real time? | Comfortably. The Izhikevich model needs only 13 operations per neuron per millisecond and reproduces all ~20 known biological firing patterns. ~1,000 neurons per ESP32 core fit inside a 1 ms tick with room to spare. |
| Does the radio have enough bandwidth? | The binding constraint, and it works: spikes are batched ~119 per 250-byte broadcast frame. An ESP32-only cluster at a high PHY rate carries on the order of 10⁵ spike events per second — enough for tens of boards at biological firing rates. |
| Can boards stay synchronized? | Broadcast frames reach all receivers within ~80 µs of each other; using hardware receive timestamps, cluster-wide sync well under 100 µs is achievable — 10% of one simulation tick. |
| Do all chip variants work? | Every Wi-Fi-equipped member of the family participates: ESP32, S2, S3, C2, C3, C5, C6, plus ESP8266 as a leaf tier. Fixed-point s16.15 arithmetic gives bit-identical results on chips with and without an FPU. |

## How big can it get?

Honest numbers, calibrated against biology:

| System | Neurons | Synapses |
|---|---|---|
| Human brain | 86,000,000,000 | ~10¹⁴–10¹⁵ |
| Fruit fly (complete connectome, 2024) | 139,255 | 54,500,000 |
| *C. elegans* worm | 302 | ~7,600 |
| **DBrain — 40-board ESP32 cluster** | **~40,000** | **~10⁷** |

A 40-board cluster sits at roughly a third of a fruit fly's brain and over a
hundred times *C. elegans* — squarely inside the range where nature produces
real, observable behavior: locomotion, avoidance, habituation, learning.

## Architecture at a glance

- **Spikes as broadcasts.** An ESP-NOW broadcast is hardware multicast: one frame,
  every subscriber. Spike bundles are fire-and-forget; losses are counted, never
  retried — a retransmitted spike would be a late spike, and late is wrong.
- **Biology absorbs the network.** Synaptic delays (1–15 ms) swallow radio latency
  and jitter entirely: a spike arriving one tick late is still applied at exactly
  the right biological time.
- **One shared tick.** A coordinator beacons time; receivers lock on with hardware
  RX timestamps and linear regression. Synchronized nodes transmit in assigned TDMA
  slots, eliminating collisions on the shared channel.
- **Self-organizing.** Heartbeat-based membership, deterministic coordinator
  election, automatic failure detection, and capability-weighted placement: a
  PSRAM-equipped S3 takes thousands of synapses; an ESP8266 takes a sensory leaf role.
- **Defined from your desk.** A host-side network compiler maps populations and
  projections onto boards, minimizing radio traffic — or, with no PC at all, the
  cluster derives an entire network deterministically from a broadcast recipe and seed.

## Project goals

The end state we are building toward — the complete platform:

1. **The full stack, working:** firmware for every supported chip, the network
   compiler, time sync, self-organization, and a live browser dashboard with spike
   rasters and cluster telemetry.
2. **A biological milestone:** the complete *C. elegans* connectome — all 302
   neurons and ~7,600 synapses of a real organism — running on the cluster and
   driving a wheeled robot's behavior.
3. **Learning in hardware:** STDP synaptic plasticity, so networks trained by
   experience rather than programming, watched live as weights change.
4. **Scale-out:** a documented, reproducible 40-board reference cluster —
   the largest radio-linked spiking network built to date.

## Status & roadmap

The research phase is **complete**; the team is actively working through implementation.

| Phase | Deliverable | Status |
|---|---|---|
| Research | Full feasibility study & system design | ✅ Complete |
| 0 — Transport | Radio benchmark suite, measured channel limits | 🔨 In progress |
| 1 — First spikes | Two boards, one network, spikes over the air | Planned |
| 2 — Time | Sub-tick synchronization + TDMA slotting | Planned |
| 3 — Self-organization | Live join/leave, election, failure recovery | Planned |
| 4 — Compiler | Define networks on a PC, deploy to the cluster | Planned |
| 5 — Dashboard | Live rasters, heatmaps, telemetry in the browser | Planned |
| 6 — Learning | STDP plasticity, rebalancing, OTA updates | Planned |

Code lands in this repository as each phase completes.

---

<div align="center">

**Ali Akrami**

[dibachain.ir](https://dibachain.ir)

</div>
