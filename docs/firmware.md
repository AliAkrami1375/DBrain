# Firmware

Two projects share the core and the console: `firmware/esp32` (ESP-IDF
v5.5, every Wi-Fi-equipped ESP32) and `firmware/esp8266` (ESP8266 RTOS SDK
v3.4). Neither uses Arduino or PlatformIO.

## 1. ESP32 family

### Requirements

- ESP-IDF v5.5.x with the toolchains for the targets you use
  (`./install.sh esp32,esp32s2,esp32s3,esp32c2,esp32c3,esp32c5,esp32c6`).
- A board with 4 MB of flash (2 MB on the ESP32-C2, which uses
  `partitions-2mb.csv`).

### Build and flash

```
. $IDF_PATH/export.sh
cd firmware/esp32
idf.py set-target esp32s3          # esp32 | esp32s2 | esp32s3 | esp32c2 | esp32c3 | esp32c5 | esp32c6
idf.py menuconfig                  # Component config → DBrain (optional)
idf.py build flash monitor
```

Per-target defaults live in `sdkconfig.defaults.<target>` (CPU frequency,
PSRAM on the S3, image buffer and neuron capacity for small chips).
Separate build directories keep several targets side by side:
`idf.py -B build-esp32c3 -D SDKCONFIG=build-esp32c3/sdkconfig set-target esp32c3 build`.

### Partition table

| Partition | Type | Offset | Size (4 MB) | Size (2 MB, C2) |
|---|---|---|---|---|
| nvs | data/nvs | 0x9000 | 24 KB | 24 KB |
| phy_init | data/phy | 0xf000 | 4 KB | 4 KB |
| factory | app | 0x10000 | 1792 KB | 1408 KB |
| artifact | data/0x40 | 0x1D0000 | 1 MB | 512 KB at 0x170000 |

### Configuration (`menuconfig → DBrain`)

| Option | Default | Runtime key | Meaning |
|---|---|---|---|
| `DBRAIN_CLUSTER_ID` | 1 | `cluster` | nodes only talk to their own cluster |
| `DBRAIN_CHANNEL` | 1 | `channel` | Wi-Fi channel 1..13; identical on every node |
| `DBRAIN_MODE_*` | recipe | `mode` (0/1) | recipe or artifact partition |
| `DBRAIN_RATE_*` | 1 Mbps | `rate` (0..3) | preferred PHY rate when no ESP8266 is present |
| `DBRAIN_SLOT_EVENTS` | 32 | `slot_ev` | events a transmission slot is sized for |
| `DBRAIN_MIN_PERIOD_US` | 1000 | `min_per` | lower bound on the tick period |
| `DBRAIN_TX_LATENCY_US` | 0 | `tx_lat` | beacon send-callback latency (phase 2 measurement) |
| `DBRAIN_DISCOVER_MS` | 5000 | `discover` | listen this long before forming a cluster |
| `DBRAIN_MAX_NEURONS` | 2000 | | arena capacity (74 bytes per neuron) |
| `DBRAIN_IMAGE_KB` | 512 | | image buffer (PSRAM when available) |
| `DBRAIN_STREAM_DEFAULT` | off | `stream` | JSON event stream at boot |
| `DBRAIN_RECIPE_*` | seed 1, random, 20 local, 10 remote, drive 1536 | `seed`, `topology`, `fan_local`, `fan_remote`, `drive` | recipe proposed when this node forms a cluster |

Runtime keys are stored in NVS with `set KEY VALUE` and override the build
defaults after a reboot; `erase` forgets them.

### Tasks

| Task | Priority | Work |
|---|---|---|
| Wi-Fi task (IDF) | high | ESP-NOW callbacks copy frames into a 32-entry queue with the hardware receive timestamp |
| `dbrain` (core 1 on dual-core chips) | 10 | drains the queues, `node_on_frame`, `node_poll` every millisecond, benchmark, PHY rate changes |
| main | 1 | console: one line in, text or JSON out |

The node task and the console share one mutex around every access to
`node_t`.

### Radio details

- Station interface, never associated; `WIFI_PS_NONE`; storage in RAM.
- Broadcast peer added at start; unicast peers are added on demand into a
  16-entry least-recently-used cache (the ESP-NOW table holds 20).
- Receive timestamps come from `rx_ctrl->timestamp` (32-bit µs, widened
  against `esp_timer_get_time()`); if a frame carries no timestamp the
  callback time is used and the frame is marked as software-stamped.
- Send callbacks are matched to frames in order through a tag FIFO; the
  node uses them for beacons only.
- When the beacon announces a rate above 1 Mbps,
  `esp_now_set_peer_rate_config` is applied to the broadcast peer and to
  every cached peer.

## 2. ESP8266

### Requirements

- ESP8266 RTOS SDK v3.4 (`IDF_PATH` pointing to it) and the
  `xtensa-lx106-elf` toolchain (gcc 8.4) on `PATH`.
- The SDK's Python requirements in a virtual environment.
- 2 MB or more of flash (NodeMCU, Wemos D1 mini, ESP-12 modules).

### Build and flash

```
export IDF_PATH=/path/to/ESP8266_RTOS_SDK
export PATH=$PATH:/path/to/xtensa-lx106-elf/bin
cd firmware/esp8266
python $IDF_PATH/tools/idf.py menuconfig
python $IDF_PATH/tools/idf.py build flash monitor
```

Partition table: nvs 0x9000, phy_init 0xf000, factory app 0x10000
(896 KB), artifact 0xF0000 (64 KB).

### What differs from the ESP32 build

- No hardware receive timestamp: every frame is stamped on callback entry,
  the node advertises that, and the coordinator widens every slot's guard
  to 1500 µs and keeps the cluster at 1 Mbps.
- Never a coordinator (leaf tier).
- `MEMBERSHIP_MAX` is 32 and two control messages may be in flight.
- Defaults: 200 neurons of arena, 16 KB image buffer. With about 45 KB of
  free heap the recipe gives such a node roughly 100 neurons.
- The artifact partition is read into the image buffer at boot instead of
  being memory-mapped.

## 3. Console

The console is the default UART (or USB-Serial-JTAG where that is the
configured console). Lines are terminated by CR or LF.

| Command | Output |
|---|---|
| `status` | node id, state, epoch, coordinator, cluster parameters, shard, sync residual |
| `members` | the membership table |
| `telemetry` | counters as one JSON object |
| `stream off \| summary \| full` | event stream mode (§4) |
| `inject N CURRENT [TICK]` | add `CURRENT` (s16.15, e.g. 327680 = 10.0) to neuron `N` at `TICK` (0 = next) |
| `pause`, `resume` | stop and restart the tick loop |
| `set KEY VALUE`, `erase` | persisted settings (table above) |
| `artifact info`, `artifact save` | describe the bound image; persist the image buffer to the artifact partition |
| `bench tx N LEN US`, `bench rx`, `bench report`, `bench stop` | transport benchmark (§5) |
| `reboot`, `version`, `help` | |

## 4. Event stream (gateway)

With `stream summary` or `stream full` the node prints one JSON object per
line, each with an `ev` field:

| `ev` | Fields |
|---|---|
| `spikes` | `node, shard, tick, n` and in full mode `idx` (first 64 indices, `truncated` when more) |
| `state` | `node, state` |
| `coordinator` | `node, epoch` |
| `member` | `node, shard, liveness` (1 alive, 2 suspect, 3 dead) |
| `shard` | `node, shard, neurons, tick` |
| `sync` | `coordinator, residual_us` |
| `resync` | `tick, skipped` |
| `joined` | `node, epoch` |
| `artifact` | `shard, status` |
| `status` (once per second) | node summary, counters and the `members` array |

`dbrain gateway --port /dev/ttyUSB0 --log run.jsonl --ws 8765` records the
stream and feeds `host/dashboard/index.html`. At 115200 baud the full
mode saturates around 100 spike lines per second; raise the console baud
rate in menuconfig (921600 works on USB-UART bridges) for busy nodes.

## 5. Transport benchmark (roadmap phase 0)

On the sender: `bench tx 2000 250 3000` sends 2000 frames of 250 bytes
every 3000 µs. On each receiver: `bench rx` before, `bench report` after.
The report lists received and lost frames (by sequence), bytes, frames and
kilobytes per second, minimum, mean and maximum inter-arrival time and
mean RSSI. Benchmark frames use type 15 and are ignored by the node, so
the cluster keeps running (and then shares the channel with the test).

## 6. Measuring `tx_latency_us` (roadmap phase 2)

Toggle a GPIO in the send callback and capture it together with a
receiver's GPIO toggled on `rx_ctrl->timestamp` arrival with a logic
analyser; the difference between the end of the frame on the air and the
callback is the latency. Store it with `set tx_lat VALUE` on coordinator
candidates. Until measured, the default of 0 leaves a constant offset
between the coordinator and the rest of the cluster; members still agree
with each other.
