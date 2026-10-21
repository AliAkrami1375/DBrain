# Testing

Everything that does not need a radio is tested on the host; the firmware
is compiled for every target. Hardware measurements are the roadmap's
phase gates and are listed at the end.

## 1. Host build and C tests

```
cmake -S . -B build -G Ninja      # Debug, AddressSanitizer and UBSan on by default
cmake --build build
ctest --test-dir build --output-on-failure
```

| Test | What it verifies |
|---|---|
| `test_fix` | platform self-test, rounding of products, saturation, constants |
| `test_neuron` | RS cell silent at rest and firing at 15..60 Hz under I = 10, bias equivalence, chattering fires more, exact initial state, bit-identical replay, clamps under absurd input, LIF refractory behaviour |
| `test_bundle` | 250-byte limit, round trip, every malformed-frame rejection, sequence-gap accounting including wrap and restart |
| `test_artifact` | builder → parser round trip, segment cutting at 256 and 255, CRC detection, every ordering rule of the builder, size arithmetic |
| `test_shard` | arena sizing, alignment and size errors, local synapses land at `fire tick + delay`, buckets consumed exactly once, the acceptance window `tick ≤ due ≤ tick + 15` on both sides, saturation, reset |
| `test_membership` | capability scoring and tiers, heartbeat round trip, table updates and versioning, rank and free ids, election comparison, suspect → dead → forgotten timing, table-full behaviour |
| `test_timesync` | exact recovery of offset and skew, error under 100 µs with 10 ppm skew and 80 µs jitter, under 1.5 ms with 1 ms jitter, filter reset on a jump, window trimming, slew and step, beacon round trip |
| `test_slot` | airtime model (250 B at 1 Mbps = 2.5..2.9 ms), slot and period arithmetic for 2 and 40 members, guard widening, minimum period |
| `test_control` | encode/decode, acknowledgement, duplicate suppression, 31 retries at 100 ms then failure, wrong-peer acks, in-flight limit, cluster isolation |
| `test_recipe` | round trip and validation, determinism (identical bytes), delays ≥ 2 off-board, fan-in within ±10 % of the recipe, roster errors, reproducible drive, a 3-node random network keeps firing and replays bit-identically, a synfire ring propagates in roster order |
| `test_node` | eight cluster scenarios on the virtual radio: lone board forms a cluster and ticks; lone leaf never promotes; four boards converge on the strongest, exchange spikes with zero late/early events and clock error under 150 µs; coordinator death → re-election with a new epoch and continued ticks; the dead board rejoins with its old id; a mixed ESP8266 cluster uses 1 Mbps, wide guards and millisecond sync; 5 % loss is counted and tolerated; a late joiner triggers warm rebuilds without losing the tick count |

All eleven pass under AddressSanitizer and UBSan.

## 2. Python tests

```
pip install -e host[test]
python -m pytest host/tests
```

| Test | What it verifies |
|---|---|
| `test_fix_parity` | `fix.mul` equals `fix_mul` on 20 000 random pairs and the edge cases |
| `test_neuron_parity` | Python and C neuron steps agree bit for bit over 5000 random inputs for RS, CH, FS, biased RS and a LIF cell |
| `test_artifact_parity` | Python-built recipe shards are byte-identical to the C builder for all three topologies; parse round trip; builder rules; a Python-built image runs in the C shard engine |
| `test_compiler` | the example compiles, every synapse is placed, cross-node delays ≥ 2, images load in C, the reference run spikes with zero late/early events, compilation and the reference are deterministic, leaf budgets are respected |

The reference simulator needs `build/core/libdbrain.so` (built without
sanitizers by the host CMake) or `DBRAIN_LIB` pointing at it.

## 3. Simulator scenarios

`build/sim/dbrain-sim` is the same harness `test_node` uses, with a
command line. Typical checks after a change to the agent:

```
dbrain-sim --nodes 8 --seconds 30                      # converged_at ≈ 3 s, 0 late/early, |sync_error_us| < 100
dbrain-sim --nodes 4 --kill-coord-at 10 --seconds 30   # reelected_at − 10 ≈ 4 s, epoch 2 everywhere
dbrain-sim --nodes 6 --loss 5 --seconds 30             # seq_gaps ≈ 5 % of bundles_rx, no ctrl_failures
dbrain-sim --nodes 3 --chips esp32,esp8266 --seconds 20
```

## 4. Firmware compilation

```
cd firmware/esp32 && for t in esp32 esp32s2 esp32s3 esp32c2 esp32c3 esp32c5 esp32c6; do
  idf.py -B build-$t -D SDKCONFIG=build-$t/sdkconfig set-target $t build || exit 1; done
cd ../esp8266 && python $IDF_PATH/tools/idf.py build
```

## 5. Hardware test plan (phase gates)

| Phase | Measurement | Tool |
|---|---|---|
| 0 transport | frames/s and loss vs payload and rate on 2..5 boards | `bench tx` / `bench rx` |
| 1 first spikes | synfire ring on two boards: `dbrain-sim --topology synfire` result reproduced over the air | `stream full` + `dbrain compare` |
| 2 time | `tx_latency_us`, cluster sync residual | GPIO + logic analyser, `status` |
| 3 self-organisation | join/leave/kill while the raster runs | dashboard |
| 4 compiler | a compiled network's raster vs the reference | `dbrain reference`, `dbrain compare` |
