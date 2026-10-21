/*
 * Per-node counters. A dropped spike is not an error; an uncounted dropped
 * spike is. Everything the node drops, skips or resynchronises shows up here,
 * is summarised in heartbeats and is streamed by the gateway.
 */
#ifndef DBRAIN_TELEMETRY_H
#define DBRAIN_TELEMETRY_H

#include <stdint.h>

typedef struct {
    /* spike plane */
    uint32_t bundles_tx;
    uint32_t bundles_rx;          /* accepted bundles from subscribed shards */
    uint32_t bundles_ignored;     /* valid bundles from shards we hold no synapses for */
    uint32_t bundles_bad;         /* malformed frames */
    uint32_t bundles_stale;       /* shard version mismatch */
    uint32_t events_tx;
    uint32_t events_rx;
    uint32_t events_applied;      /* synapses applied into buckets */
    uint32_t events_late;         /* delivery tick already integrated */
    uint32_t events_early;        /* delivery tick beyond the delay ring */
    uint32_t events_norow;        /* events from a subscribed shard whose neuron has no row here */
    uint32_t seq_gaps;            /* bundles lost, by sequence number */

    /* time */
    uint32_t tick_overruns;       /* tick integration finished after the next tick began */
    uint32_t tick_resyncs;        /* local tick stepped to the coordinator's */
    int32_t  sync_residual_us;    /* last beacon: estimate minus observation */
    uint32_t sync_samples;

    /* membership and control plane */
    uint32_t heartbeats_rx;
    uint32_t control_retries;
    uint32_t control_failures;
    uint32_t elections;           /* times this node promoted itself */

    /* resources */
    uint32_t heap_low_water;
    uint32_t frames_dropped_rx;   /* receive queue full */
} telemetry_t;

static inline void telemetry_reset(telemetry_t *t)
{
    uint32_t *p = (uint32_t *)t;
    unsigned i;
    for (i = 0; i < sizeof(*t) / sizeof(uint32_t); i++) p[i] = 0;
}

#endif /* DBRAIN_TELEMETRY_H */
