/*
 * Compilerless networks: a shard derived from a recipe.
 *
 * With no host in the loop the coordinator hands every member a 16-byte
 * recipe. Each node derives its own population and every synaptic row it
 * owns from (recipe, source node id, source size, own node id, own size),
 * so the whole network is a pure function of the recipe and the roster. No
 * tables are shipped. The host reference derives the same shards from the
 * same inputs, which is what makes the result checkable.
 *
 * Shard id equals node id in recipe mode. A node learns a source's size
 * from that source's heartbeat (n_neurons), so two nodes always agree on the
 * rows between them once they have heard each other.
 *
 * Wire layout, 16 bytes (docs/protocol.md section 3.5):
 *   0  u32 seed
 *   4  u8  topology (RECIPE_*)
 *   5  u8  exc_percent      excitatory share of each population, 0..100
 *   6  i16 w_exc            weight of excitatory synapses, shift 8 (128 = 1.0)
 *   8  i16 w_inh            weight of inhibitory synapses
 *   10 u16 local_fan_in     on-board synapses per neuron
 *   12 u16 remote_fan_in    total off-board synapses per neuron, split across sources
 *   14 u16 drive            peak of the per-tick random input, shift 8 (128 = 1.0)
 */
#ifndef DBRAIN_RECIPE_H
#define DBRAIN_RECIPE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dbrain/membership.h"
#include "dbrain/shard.h"

#define RECIPE_LEN 16u

enum {
    RECIPE_RANDOM  = 0,  /* uniform random recurrent network across all nodes */
    RECIPE_SPATIAL = 1,  /* as above, fan-out falls with the distance between node coordinates */
    RECIPE_SYNFIRE = 2   /* members in id order form a ring; each pool drives the next */
};

typedef struct {
    uint32_t seed;
    uint8_t  topology;
    uint8_t  exc_percent;
    int16_t  w_exc;
    int16_t  w_inh;
    uint16_t local_fan_in;
    uint16_t remote_fan_in;
    uint16_t drive;
} recipe_t;

typedef struct {
    uint16_t node_id;
    uint16_t n_neurons;
} recipe_member_t;

void recipe_default(recipe_t *r);
void recipe_encode(uint8_t *out, const recipe_t *r);
bool recipe_decode(const uint8_t *in, recipe_t *r);

/* Neurons a node of this capability simulates under the recipe. */
uint16_t recipe_neurons_for(const recipe_t *r, const cap_t *cap);

/* Excitatory count of a population of n neurons. */
uint16_t recipe_exc_count(const recipe_t *r, uint16_t n);

/*
 * Builds the artifact of node `self` into buf. members must be sorted by
 * ascending node id and include self. Returns the image size or an
 * ARTIFACT_E_* code.
 */
int32_t recipe_build(const recipe_t *r, uint16_t self_id, uint16_t self_version,
                     const recipe_member_t *members, uint16_t n_members, void *buf, uint32_t cap);

/*
 * Per-tick background input: every neuron of the shard receives a current
 * drawn uniformly in [0, drive) for the tick about to be integrated. The
 * draw depends only on (seed, shard id, tick), so the host can replay it.
 */
void recipe_drive(const recipe_t *r, shard_t *sh);

#endif /* DBRAIN_RECIPE_H */
