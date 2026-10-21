# Host tools

The Python package in `host/` defines networks, places them on a cluster,
runs them on the host with the same arithmetic as the boards, and talks
to a gateway board.

```
pip install -e host            # add [serial,dashboard] for the gateway
cmake -S . -B build && cmake --build build     # builds libdbrain.so for the reference simulator
```

## 1. Describing a network

```python
from dbrain.network import Network, FixedFanIn, AllToAll, FixedProbability, OneToOne, FromList
from dbrain.neuron import RS, FS, Params

network = Network("demo")
stim = network.population("stim", 20, RS, rate_hz=20.0, stimulus=True)
pool = network.population("pool", 400, RS, rate_hz=5.0)
inh  = network.population("inh", 100, Params.izh(0.1, 0.2, -65, 2), rate_hz=10.0)
network.connect(stim, pool, AllToAll(), weight=2.0, delay=1, seed=1)
network.connect(pool, pool, FixedFanIn(30), weight=0.5, delay=(1, 4), seed=2)
network.connect(pool, inh, FixedProbability(0.1), weight=1.0, delay=2, seed=3)
network.connect(inh, pool, FixedFanIn(10), weight=-3.0, delay=(1, 2), seed=4)
```

- `Params.izh(a, b, c, d, i_bias)` and `Params.lif(v_rest, v_reset,
  v_thresh, tau_m_ms, r_m, refrac_ticks, i_bias)` build parameter blocks;
  `RS, IB, CH, FS, LTS` are the standard Izhikevich cells.
- Weights are in model units (1.0 = 128 at the default weight shift of
  8, range ±256); delays are ticks, a `(lo, hi)` range, or a callable.
- `rate_hz` is the expected mean rate the placer uses to estimate radio
  traffic; `stimulus=True` marks populations the host drives.
- Connectivity is materialised deterministically from the projection seeds.

## 2. Compiling

```
dbrain compile network.py -n nodes.json -o build-net [--cluster 1] [--weight-shift 8]
```

`nodes.json` lists the boards:

```json
[{"node_id": 1, "chip": "esp32s3", "free_heap": 250000, "psram": 4194304},
 {"node_id": 2, "chip": "esp32c3", "free_heap": 200000, "max_neurons": 1000, "image_bytes": 1048576}]
```

`free_heap` is the value `status` prints on the board; `max_neurons` and
`image_bytes` default to 2000 and 1 MB (the ESP32 partition).

Placement:

1. Budgets: a node takes at most `min(max_neurons, free_heap / 2 / 74)`
   neurons; an image must fit `image_bytes`; leaf chips (ESP8266, C2) do
   not take populations larger than half their budget when other nodes
   exist.
2. Cost of an assignment: expected cross-node frames per second, counted
   once per (source population, node that holds any of its targets) pair
   as `min(size × rate_hz, 1000) / 119`, plus a small load-balance term
   (neurons per unit of capability, where capability is MHz × 1.5 on
   dual-core chips).
3. Populations are placed largest first at minimum cost, then single
   populations are moved while the traffic strictly decreases.
4. Within a node, populations keep the order of the script; neuron `i` of
   a population is local index `first + i`. Cross-node delays below 2 are
   raised to 2 (reported as a warning); weights are rounded to int16.

Output: `shard_<node>.bin` per node and `manifest.json` with the placement
(`populations → node, first, size`), per-node counts, the traffic
estimate and warnings. Compilation is deterministic.

## 3. Running on the host

```
dbrain reference build-net --ticks 2000 --raster raster.csv \
       --stimulus '{"node": 1, "neurons": [0,1,2,3], "current": 491520, "ticks": [0, 50]}'
```

Runs every shard of the build through the C core with perfect delivery
and writes `(tick, node, neuron)` rows. `current` is s16.15
(491520 = 15.0). `dbrain compare raster.csv run.jsonl --tick-offset T`
matches the reference against a gateway log recorded with `stream full`
(`T` aligns the cluster's tick numbering with the reference's tick 0).

The reference is what the cluster computes when nothing is lost; a
difference beyond the stimulus timing points at loss (`seq_gaps`), late
or early events, or a model-version mismatch.

## 4. Flashing a shard

```
dbrain flash-artifact --port /dev/ttyUSB0 --chip esp32s3 build-net/shard_1.bin
```

validates the image and runs `esptool.py write_flash` at the artifact
partition offset (0x1D0000 on 4 MB ESP32 layouts, 0x170000 on the C2,
0xF0000 on the ESP8266). Set the board to artifact mode (`set mode 1`,
reboot). The board's node id must equal the shard id the image was
compiled for; the coordinator keeps ids stable, and a board that joins
with a held shard asks for its id as the preferred one.

## 5. Gateway and dashboard

```
dbrain send --port /dev/ttyUSB0 "stream full"
dbrain gateway --port /dev/ttyUSB0 --log run.jsonl --ws 8765
open host/dashboard/index.html
```

The dashboard draws a raster of the spike events it receives (one colour
per node), the gateway node's status and the membership table. It is a
single static page; the WebSocket address is editable.

## 6. Bit-exact mirrors

`dbrain.fix`, `dbrain.rng`, `dbrain.neuron`, `dbrain.artifact` and
`dbrain.recipe` reimplement the core in Python. They exist so that the
compiler can write images without the C library and so that the model is
specified twice independently; `host/tests` checks them against the C
core through `dbrain.core` (ctypes).

## 7. Cluster simulator

`build/sim/dbrain-sim` runs real node agents on a virtual radio cell:

```
dbrain-sim --nodes 4 --chips esp32c3,esp32s3,esp32,esp32 --seconds 15 --loss 2 --kill-coord-at 8
dbrain-sim --nodes 3 --chips esp32,esp8266 --topology synfire --raster 200
```

Options: `--rate` (1, 6, 24, 54 Mbps), `--join-at S` (power the last
board on late), `--seed`, `--verbose` (node logs), `--trace` (tick lag
episodes). The JSON summary reports convergence time, the coordinator,
re-election time after a kill, frames, collisions and losses, and per
node: state, id, epoch, shard size, tick, clock error against the
coordinator, spikes, bundle and event counters, late and early events,
sequence gaps, overruns and resyncs.
