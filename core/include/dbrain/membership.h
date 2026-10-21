/*
 * Membership: who is in the cluster, what each node can do, who leads.
 *
 * All membership state derives from broadcast heartbeats. Every node
 * overhears every heartbeat, keeps a table of members keyed by MAC address,
 * detects failures by missed heartbeats (SWIM style: suspect, probe,
 * dead) and derives the coordinator by the deterministic rule of
 * docs/protocol.md section 5: the highest (epoch, capability score, MAC)
 * wins. This module is pure bookkeeping; the node agent drives it with
 * timestamps and frames and acts on what it reports.
 */
#ifndef DBRAIN_MEMBERSHIP_H
#define DBRAIN_MEMBERSHIP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dbrain/bundle.h"

#ifndef MEMBERSHIP_MAX
#define MEMBERSHIP_MAX 64
#endif

#define NODE_ID_NONE   0u
#define NODE_ID_MAX    4095u
#define SHARD_NONE     0xFFFFu

/* Chip identifiers carried in capability records. */
enum {
    DBRAIN_CHIP_UNKNOWN = 0, DBRAIN_CHIP_ESP8266 = 1, DBRAIN_CHIP_ESP32 = 2, DBRAIN_CHIP_ESP32S2 = 3, DBRAIN_CHIP_ESP32S3 = 4,
    DBRAIN_CHIP_ESP32C2 = 5, DBRAIN_CHIP_ESP32C3 = 6, DBRAIN_CHIP_ESP32C5 = 7, DBRAIN_CHIP_ESP32C6 = 8, DBRAIN_CHIP_HOST = 15
};

enum { TIER_LEAF = 0, TIER_MID = 1, TIER_WORKHORSE = 2, TIER_HEAVY = 3 };

#define CAP_FLAG_FPU       0x01u
#define CAP_FLAG_HW_RX_TS  0x02u   /* hardware receive timestamp available */
#define CAP_FLAG_ELIGIBLE  0x04u   /* may act as coordinator */
#define CAP_FLAG_PSRAM     0x08u

/* 16 bytes on the wire, see docs/protocol.md section 3.1. */
typedef struct {
    uint8_t  chip;
    uint8_t  cores;
    uint16_t mhz;
    uint32_t free_heap;
    uint32_t psram;
    uint8_t  flags;
    uint8_t  tier;
    uint16_t reserved;
} cap_t;

#define CAP_LEN 16u

void cap_encode(uint8_t *p, const cap_t *c);
void cap_decode(const uint8_t *p, cap_t *c);

/* Static capability score used as the election key prefix (never uses free_heap). */
uint32_t cap_score(const cap_t *c);

/* Neurons a node of this capability takes in recipe mode, given the fan-in. */
uint16_t cap_neuron_budget(const cap_t *c, uint16_t fan_in);

/* Fills chip-derived defaults (tier, eligibility, timestamp flag). */
void cap_classify(cap_t *c);

/* Node states reported in heartbeats. */
enum {
    NODE_STATE_BOOT = 0,
    NODE_STATE_DISCOVER = 1,   /* listening for a cluster */
    NODE_STATE_JOINING = 2,    /* join request in flight */
    NODE_STATE_MEMBER = 3,
    NODE_STATE_COORDINATOR = 4
};

#define HB_FLAG_COORDINATOR 0x01u
#define HB_FLAG_HAS_SHARD   0x02u
#define HB_FLAG_RECIPE      0x04u   /* shard generated from the cluster recipe */

/* Heartbeat payload, 56 bytes on the wire (docs/protocol.md section 3.2). */
typedef struct {
    uint16_t cluster_id;
    uint16_t node_id;
    uint8_t  mac[6];
    uint16_t epoch;
    uint16_t coord_id;
    uint16_t shard_id;
    uint16_t shard_version;
    uint16_t n_neurons;
    cap_t    cap;
    uint8_t  state;
    uint8_t  flags;
    uint16_t hb_seq;
    uint32_t tick;
    uint16_t drops;
    uint16_t overruns;
    int16_t  sync_residual_us;
    uint16_t reserved;
} heartbeat_t;

#define HEARTBEAT_LEN 56u

size_t heartbeat_encode(uint8_t *out, size_t cap, const heartbeat_t *h);
bool   heartbeat_decode(const uint8_t *in, size_t len, heartbeat_t *h);

/* Liveness as judged by this node. */
enum { MEMBER_EMPTY = 0, MEMBER_ALIVE = 1, MEMBER_SUSPECT = 2, MEMBER_DEAD = 3 };

typedef struct {
    uint8_t  mac[6];
    uint8_t  liveness;        /* MEMBER_* */
    uint8_t  node_state;      /* NODE_STATE_* as reported */
    uint16_t node_id;
    uint16_t epoch;
    uint16_t coord_id;
    uint16_t shard_id;
    uint16_t shard_version;
    uint16_t n_neurons;
    cap_t    cap;
    uint8_t  flags;           /* HB_FLAG_* */
    uint8_t  probes;          /* probes sent while suspect */
    uint16_t hb_seq;
    uint32_t tick;
    uint32_t last_heard_ms;
    uint32_t suspect_since_ms;
    uint16_t drops;
    uint16_t overruns;
    int16_t  sync_residual_us;
    seq_tracker_t bundle_seq;
    uint16_t gaps;            /* bundles lost from this member, as seen here */
} member_t;

typedef struct {
    member_t m[MEMBERSHIP_MAX];
    uint16_t count;           /* occupied entries */
    uint32_t version;         /* bumps on every liveness or id change */
} membership_t;

/* Timing constants (milliseconds). */
#define HB_INTERVAL_MS      1000u
#define HB_JITTER_MS        100u   /* ± */
#define HB_SUSPECT_MS       (3u * HB_INTERVAL_MS)
#define HB_PROBE_TIMEOUT_MS 2000u
#define HB_DEAD_RETAIN_MS   30000u

void membership_init(membership_t *t);

/* Records a heartbeat. Returns the entry, or NULL if the table is full. */
member_t *membership_update(membership_t *t, const heartbeat_t *h, uint32_t now_ms);

member_t *membership_find_mac(membership_t *t, const uint8_t mac[6]);
member_t *membership_find_id(membership_t *t, uint16_t node_id);

/*
 * Advances liveness by time. Entries silent for HB_SUSPECT_MS become
 * suspect; a suspect entry whose probe timed out becomes dead; dead entries
 * are dropped after HB_DEAD_RETAIN_MS. Returns true when anything changed.
 * Newly suspected entries have probes == 0 so the caller can probe them.
 */
bool membership_tick(membership_t *t, uint32_t now_ms);

/* Marks an entry alive again (a probe answered). */
void membership_confirm(membership_t *t, member_t *m, uint32_t now_ms);

/* Marks an entry dead now (a probe cycle failed). */
void membership_declare_dead(membership_t *t, member_t *m, uint32_t now_ms);

/* Number of alive or suspect entries holding a node id. */
uint16_t membership_active(const membership_t *t);

/* Rank of node_id among active ids in ascending order, or -1. */
int membership_rank(const membership_t *t, uint16_t node_id);

/* Smallest node id not held by any active or dead entry, or NODE_ID_NONE. */
uint16_t membership_free_id(const membership_t *t);

uint16_t membership_max_epoch(const membership_t *t);

/*
 * Hash of the active roster as the recipe sees it: (node id, neuron count)
 * of every active member running the recipe. Changes only when a shard
 * must be rebuilt.
 */
uint32_t membership_roster_signature(const membership_t *t);

/* Hash of what the slot plan depends on: active count, chips and timestamp flags. */
uint32_t membership_plan_signature(const membership_t *t);

/*
 * Election key comparison: returns >0 when (epoch_a, score_a, mac_a) beats
 * (epoch_b, score_b, mac_b), <0 when it loses, 0 when identical.
 */
int election_compare(uint16_t epoch_a, uint32_t score_a, const uint8_t mac_a[6],
                     uint16_t epoch_b, uint32_t score_b, const uint8_t mac_b[6]);

/*
 * The member that should coordinate according to what this node has heard:
 * the eligible, non-dead entry with the highest key, or NULL. Includes
 * only entries that claim to be coordinator or have no coordinator; a
 * member following someone else is not a candidate.
 */
member_t *membership_best_coordinator(membership_t *t);

#endif /* DBRAIN_MEMBERSHIP_H */
