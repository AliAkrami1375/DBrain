/*
 * Spike bundle: the one frame type that carries the simulation.
 *
 *   offset  size  field
 *   0       1     FRAME_MAGIC
 *   1       1     version | FRAME_BUNDLE
 *   2       2     source node id
 *   4       4     tick in which the events fired
 *   8       2     per-node bundle sequence number
 *   10      1     event count, 0..119
 *   11      1     flags
 *   12      2*n   events: local neuron indices on the source shard
 *
 * 12-byte header plus 119 two-byte events fills the 250-byte limit. Bundles
 * are broadcast, never acknowledged and never retransmitted; receivers count
 * sequence gaps as loss. A node whose tick produced more than 119 spikes
 * sends several bundles with BUNDLE_FLAG_MORE set on all but the last.
 */
#ifndef DBRAIN_BUNDLE_H
#define DBRAIN_BUNDLE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define BUNDLE_HDR_LEN     12u
#define BUNDLE_MAX_EVENTS  119u
#define BUNDLE_FLAG_MORE   0x01u

typedef struct {
    uint16_t src_node;
    uint32_t tick;
    uint16_t seq;
    uint8_t  count;
    uint8_t  flags;
    const uint8_t *events;   /* little-endian u16 array, use bundle_event() */
} bundle_view_t;

/* Returns the encoded length, or 0 if count or cap is out of range. */
size_t bundle_encode(uint8_t *out, size_t cap, uint16_t src_node, uint32_t tick, uint16_t seq,
                     uint8_t flags, const uint16_t *events, uint8_t count);

/* Returns true and fills v when buf holds a well-formed bundle. */
bool bundle_decode(const uint8_t *buf, size_t len, bundle_view_t *v);

static inline uint16_t bundle_event(const bundle_view_t *v, uint8_t i)
{
    return (uint16_t)(v->events[2 * i] | ((uint16_t)v->events[2 * i + 1] << 8));
}

/*
 * Sequence-gap accounting for one source. Returns the number of bundles
 * missing between the last seen and this sequence number (0 when in order).
 * A first observation returns 0. Reordering cannot happen on a single
 * broadcast channel, so anything but +1 is loss; a large backwards jump is
 * treated as a source reboot and resets the tracker.
 */
typedef struct {
    uint16_t last_seq;
    bool     seen;
} seq_tracker_t;

uint16_t seq_tracker_observe(seq_tracker_t *t, uint16_t seq);

#endif /* DBRAIN_BUNDLE_H */
