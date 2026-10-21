# Simulation core

Model version 1. Every statement here is what `core/src/{fix,neuron,
synapse,shard,recipe}.c` compute; `host/dbrain/{fix,neuron,artifact,
recipe}.py` reproduce them bit for bit and the test suite checks both
against each other.

## 1. Arithmetic

All simulation state is `accum_t`, a 32-bit s16.15 fixed-point number:
one sign bit, 16 integer bits, 15 fraction bits, range `[-65536, 65536)`,
resolution `2^-15 ≈ 3.05 × 10^-5`.

| Operation | Definition |
|---|---|
| `fix_mul(a, b)` | `trunc32((a·b + 2^14) >> 15)` with a 64-bit product and an arithmetic shift: round half up, wrap on overflow |
| `fix_mul_sat`, `fix_add_sat`, `fix_sub_sat` | the same, saturating to `INT32_MIN..INT32_MAX` |
| `FIX_RATIO(n, d)` | nearest representable `n/d`, ties away from zero |
| `fix_to_int` | floor |

Platform requirements (checked at start-up by `fix_selftest`): two's
complement integers and arithmetic right shifts. Every supported compiler
satisfies both; floating point is never used.

## 2. Izhikevich neuron

Parameters `a, b, c, d` and a constant bias `i_bias`, all s16.15. State
`(v, u)`, 8 bytes. One tick is 1 ms of biological time.

```
I  = clamp(input + i_bias, −10000, 10000)
repeat twice (0.5 ms Euler half steps for v):
    vc = clamp(v, −200, 1000)
    dv = 0.04·vc² + 5·vc + 140 − u + I      (0.04 is 1311/32768 = 0.040008)
    v  = clamp(vc + 0.5·dv, −200, 1000)
u = u + a·(b·v − u)
if v ≥ 30:  v = c;  u = u + d;  spike
```

The clamps only engage far outside the physiological range; they exist so
that `0.04·v²` cannot leave the s16.15 range. The square is computed as
`(0.04·vc)·vc`, saturating adds everywhere. Initial state: `v = c`,
`u = b·c`.

Standard cell types: RS `(0.02, 0.2, −65, 8)`, IB `(0.02, 0.2, −55, 4)`,
CH `(0.02, 0.2, −50, 2)`, FS `(0.1, 0.2, −65, 2)`, LTS `(0.02, 0.25, −65, 2)`.
A regular-spiking cell driven by `I = 10` fires between 15 and 60 times per
second.

## 3. Leaky integrate-and-fire

Parameters `v_rest, v_reset, v_thresh, alpha = dt/τ_m, beta = alpha·R_m,
refrac_ticks`. `u` holds the remaining refractory ticks.

```
if u > 0:  u = u − 1;  v = v_reset;  no spike
else:
    v = v + alpha·(v_rest − v) + beta·I
    if v ≥ v_thresh:  v = v_reset;  u = refrac_ticks;  spike
```

## 4. Synapses and delivery

A synapse is one 32-bit word: weight int16 (bits 31..16), delay 1..15
ticks (15..12), flags (11..8; bit 8 reserved for plasticity), target offset
(7..0). A row holds every synapse one source neuron drives on this shard,
as segments `[base u16 | count u8 | weight_shift u8]` followed by `count`
words; the effective input is `weight << weight_shift` (shift 8 by
default, so 128 = 1.0). Segments start a new base whenever a target is 256
or more above the current base or the segment holds 255 synapses, which
lets 8-bit offsets address shards of up to 65535 neurons.

Delivery of an event that fired in tick `S` with delay `d`, at a shard
whose next tick to integrate is `T`:

```
D = S + d
if D < T:        late,   counted, dropped
if D ≥ T + 16:   early,  counted, dropped
else:            bucket[D mod 16][target] += weight (saturating)
```

A tick integrates every neuron with `input = bucket[T mod 16][i]`, clears
that bucket, advances `T`, then delivers the shard's own spikes through
its own rows with `S = T − 1`. On-board synapses may use delay 1; the
compiler and the recipe give cross-shard synapses delay ≥ 2, which makes
the acceptance window tolerate one tick of skew in either direction.

External input (`shard_inject`, the `INJECT` control message, the recipe
drive) adds into the bucket of a tick in the same window.

## 5. Recipe networks

Given recipe `R`, the roster `M = [(node id, n) …]` sorted by id, and this
node's id `s` with size `n_s`:

1. Populations: `exc = n_s × exc_percent / 100` regular-spiking cells,
   then `n_s − exc` fast-spiking cells.
2. Size of a node: `cap_neuron_budget(cap, local_fan_in + remote_fan_in)`,
   capped by its arena: half the free heap divided by the per-neuron cost
   (74 bytes of state and buckets plus 4.4 bytes per synapse unless the
   rows live in PSRAM), clamped to 16..2000.
3. For every source node `k` in roster order, fan-out per source neuron:
   - same node: `local_fan_in × n_s / n_k` (rounded),
   - random: `(remote_fan_in / (|M| − 1)) × n_s / n_k`,
   - spatial: the random value scaled by `(768 − dist) / 768`, where each
     node sits on a 1024 × 1024 grid at `mix(seed ^ id·0x9E3779B1)`,
   - synfire: `remote_fan_in × n_s / n_k` only when `k` immediately
     precedes `s` in roster order (ring).
4. For source neuron `i` of node `k`: `rng = seed(R.seed, k<<16 | i, s)`;
   draw `fan_out` targets uniformly in `[0, n_s)`, sort them, then for each
   draw a delay uniformly in `[delay_min, 8]` with `delay_min = 1` on-board
   and 2 off-board; weight `w_exc` if `i < exc_k` else `w_inh`.
5. Drive: at tick `t`, `rng = seed(R.seed ^ 0xD71F, s, t)`; every neuron
   receives `rng.below(drive << 8)` in s16.15 (uniform in `[0, drive/128)`).

The generator is xorshift32 seeded by three Murmur3 finaliser passes;
`below(n)` is `(next() × n) >> 32`.

## 6. Determinism

Two nodes of different chips running the same shard image from the same
tick with the same inputs produce identical `(v, u)` sequences and
identical spike times. The host reference simulator uses the same C code;
`host/dbrain/neuron.py` is an independent implementation checked against
it over thousands of random inputs. Any change to the scheme above is a
new `DBRAIN_MODEL_VERSION`, and the artifact loader refuses images of
another model version.
