/*
 * Shard engine: the simulation state of one shard on one node.
 *
 * Time-driven neurons, event-driven synapses. Every tick integrates all
 * neurons of the shard once; synaptic work happens only when a spike
 * arrives, local or over the air. Synaptic input is accumulated in a ring of
 * SHARD_DELAY_SLOTS per-tick buckets indexed by delivery tick.
 *
 * Tick bookkeeping: shard->tick is the next tick to integrate, so ticks
 * 0 .. tick-1 are complete. An event that fired in tick S through a synapse
 * of delay d is due at D = S + d and is accepted iff
 *
 *     tick <= D <= tick + SHARD_DELAY_SLOTS - 1
 *
 * Earlier is late (already integrated) and later would alias a live bucket;
 * both are counted and dropped, never applied at the wrong time. With a
 * minimum cross-node delay of 2 ticks this tolerates one full tick of
 * skew between sender and receiver in either direction.
 */
#ifndef DBRAIN_SHARD_H
#define DBRAIN_SHARD_H

#include <stdint.h>
#include <stddef.h>
#include "dbrain/artifact.h"
#include "dbrain/telemetry.h"

#define SHARD_DELAY_SLOTS 16u

typedef struct {
    artifact_view_t art;
    neuron_state_t *state;    /* n_neurons */
    accum_t *buckets;         /* SHARD_DELAY_SLOTS × n_neurons, [slot][neuron] */
    uint16_t *fired;          /* indices that fired in the last integrated tick */
    uint16_t n_fired;
    uint32_t tick;            /* next tick to integrate */
    telemetry_t *tm;
    telemetry_t tm_own;
} shard_t;

/* Bytes of working memory a shard with n neurons needs. */
size_t shard_arena_size(uint16_t n_neurons);

/*
 * Validates the image, carves the arena and resets the state. tm may be
 * NULL, in which case the shard keeps its own counters. Returns ARTIFACT_OK
 * or an ARTIFACT_E_* code; ARTIFACT_E_NOSPACE when the arena is too small.
 */
int shard_bind(shard_t *sh, const void *image, uint32_t size, void *arena, size_t arena_size,
               telemetry_t *tm);

/* Initial neuron state, empty buckets, tick 0. */
void shard_reset(shard_t *sh);

/*
 * Swaps in a new image. When the neuron count and population layout are
 * unchanged the neuron state, buckets and tick counter survive (a warm
 * rebuild, used when the roster of a recipe network changes); otherwise it
 * behaves like shard_bind with the existing arena. Returns ARTIFACT_OK or an
 * error code; on error the shard is left unbound.
 */
int shard_rebind(shard_t *sh, const void *image, uint32_t size);

/*
 * Integrates the next tick. Afterwards sh->fired[0..n_fired) lists the
 * neurons that spiked, on-board synapses of those spikes are already queued,
 * and sh->tick has advanced. Returns n_fired.
 */
uint16_t shard_tick(shard_t *sh);

/* True when this shard holds synapses driven by neurons of src_shard. */
int shard_subscribes(const shard_t *sh, uint16_t src_shard);

/* Applies events that fired on src_shard in tick src_tick. */
void shard_deliver(shard_t *sh, uint16_t src_shard, uint32_t src_tick, const uint16_t *events,
                   uint16_t n);

/* Accessors for bindings that treat shard_t as opaque. */
size_t          shard_sizeof(void);
uint32_t        shard_tick_get(const shard_t *sh);
const uint16_t *shard_fired_get(const shard_t *sh, uint16_t *n);
telemetry_t    *shard_telemetry(shard_t *sh);
const neuron_state_t *shard_state_get(const shard_t *sh);
uint16_t        shard_neurons(const shard_t *sh);
uint16_t        shard_id(const shard_t *sh);

/* External stimulus into one neuron's bucket for tick at_tick. Same window rule. */
int shard_inject(shard_t *sh, uint16_t neuron, accum_t current, uint32_t at_tick);

#endif /* DBRAIN_SHARD_H */
