/*
 * Packed synapse word and synaptic row layout.
 *
 * A synapse is one 32-bit word, the SpiNNaker layout:
 *
 *   bits 31..16  weight, int16, in units of 2^-(15 - weight_shift) where
 *                weight_shift comes from the enclosing segment header
 *   bits 15..12  delay in ticks, 1..15 (0 is invalid)
 *   bits 11..8   flags (SYN_FLAG_*)
 *   bits  7..0   target neuron offset from the segment base
 *
 * A synaptic row holds every synapse on this node that a given source
 * neuron drives. It is a sequence of segments; each segment starts with a
 * header word and is followed by `count` synapse words:
 *
 *   bits 31..16  base   first local neuron index covered by the segment
 *   bits 15..8   count  number of synapse words, 1..255
 *   bits  7..0   weight_shift, 0..15
 *
 * Splitting a row into segments is what lets an 8-bit target field address
 * shards of up to 65535 neurons. The compiler sorts a row by target and cuts
 * a new segment whenever a target is 256 or more above the current base or
 * the segment is full.
 *
 * The effective synaptic input added to the target's bucket is
 *   (accum_t)weight << weight_shift
 * so with the default shift of 8 a weight of 128 equals 1.0 in s16.15.
 */
#ifndef DBRAIN_SYNAPSE_H
#define DBRAIN_SYNAPSE_H

#include <stdint.h>
#include "dbrain/fix.h"

#define SYN_DELAY_MIN        1u
#define SYN_DELAY_MAX        15u
#define SYN_XNODE_DELAY_MIN  2u   /* compiler rule for cross-shard projections */
#define SYN_SEG_MAX_COUNT    255u
#define SYN_WEIGHT_SHIFT_DEFAULT 8u
#define SYN_WEIGHT_SHIFT_MAX 15u

#define SYN_FLAG_PLASTIC 0x1u     /* reserved for the 64-bit plastic layout */

static inline uint32_t syn_pack(int16_t weight, uint8_t delay, uint8_t flags, uint8_t target_off)
{
    return ((uint32_t)(uint16_t)weight << 16) | ((uint32_t)(delay & 0xF) << 12) |
           ((uint32_t)(flags & 0xF) << 8) | (uint32_t)target_off;
}

static inline int16_t syn_weight(uint32_t w)   { return (int16_t)(uint16_t)(w >> 16); }
static inline uint8_t syn_delay(uint32_t w)    { return (uint8_t)((w >> 12) & 0xF); }
static inline uint8_t syn_flags(uint32_t w)    { return (uint8_t)((w >> 8) & 0xF); }
static inline uint8_t syn_target(uint32_t w)   { return (uint8_t)(w & 0xFF); }

static inline uint32_t syn_seg_pack(uint16_t base, uint8_t count, uint8_t weight_shift)
{
    return ((uint32_t)base << 16) | ((uint32_t)count << 8) | (uint32_t)weight_shift;
}

static inline uint16_t syn_seg_base(uint32_t h)  { return (uint16_t)(h >> 16); }
static inline uint8_t  syn_seg_count(uint32_t h) { return (uint8_t)((h >> 8) & 0xFF); }
static inline uint8_t  syn_seg_shift(uint32_t h) { return (uint8_t)(h & 0xFF); }

static inline accum_t syn_weight_accum(int16_t weight, uint8_t weight_shift)
{
    return (accum_t)((int32_t)weight * (int32_t)((uint32_t)1 << weight_shift));
}

/* Global neuron identity: (shard id, local index). */
static inline uint32_t gid_make(uint16_t shard, uint16_t local) { return ((uint32_t)shard << 16) | local; }
static inline uint16_t gid_shard(uint32_t gid) { return (uint16_t)(gid >> 16); }
static inline uint16_t gid_local(uint32_t gid) { return (uint16_t)gid; }

#endif /* DBRAIN_SYNAPSE_H */
