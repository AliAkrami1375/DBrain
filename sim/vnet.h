/*
 * Virtual radio cell for running real node agents on the host.
 *
 * Every node is the unmodified node_t from the core with a platform made of
 * a skewed local clock, a fake capability record and a send hook into the
 * shared channel. The channel models what matters for the protocol: frame
 * airtime at the PHY rate, CSMA deferral with a random backoff, collisions
 * between overlapping transmissions (both frames are lost), a per-receiver
 * loss probability, the receiver spread of a broadcast (every receiver gets
 * the frame within VNET_SPREAD_US of each other) and the difference between
 * a hardware receive timestamp and a software one. Unicast frames reach only
 * their destination. Time is discrete-event; nodes poll every VNET_POLL_US.
 */
#ifndef DBRAIN_VNET_H
#define DBRAIN_VNET_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dbrain/node.h"
#include "dbrain/rng.h"

#define VNET_MAX_NODES     64
#define VNET_POLL_US       250u
#define VNET_SPREAD_US     80u
#define VNET_SOFT_TS_US    1000u   /* software timestamp delay, uniform 0..this */
#define VNET_CB_LATENCY_US 60u     /* send callback after the frame left the air */
#define VNET_BACKOFF_US    310u
#define VNET_CLOCK_BASE_US (1000000000000ull)   /* local clocks start here so offsets may be negative */

typedef struct vnet vnet_t;

typedef struct {
    node_t   node;
    node_config_t cfg;
    uint8_t  mac[6];
    cap_t    cap;
    int32_t  clock_ppm;
    int64_t  clock_offset_us;
    bool     powered;
    bool     hw_ts;
    int      index;
    vnet_t  *net;
    void    *arena;
    size_t   arena_size;
    uint8_t *image;
    uint32_t image_cap;
    /* counters kept by the harness */
    uint32_t frames_tx;
    uint32_t frames_rx;
    uint32_t frames_lost;
    uint32_t spikes;
    uint32_t resyncs;
    int32_t  last_residual_us;
    uint32_t joined_at_ms;      /* first time it became member or coordinator, 0 = never */
    uint32_t coordinator_changes;
    bool     lagging;           /* trace state */
} vnode_t;

typedef struct {
    uint64_t at_us;
    uint8_t  kind;              /* 0 poll, 1 deliver, 2 sent callback */
    int16_t  node;
    int16_t  from;
    uint8_t  tag;
    bool     collided;
    uint32_t tx_id;
    size_t   len;
    uint8_t  frame[250];
} vevent_t;

typedef struct {
    uint64_t start_us, end_us;
    uint32_t id;
    bool     collided;
} vair_t;

struct vnet {
    vnode_t *nodes[VNET_MAX_NODES];
    int      n_nodes;
    uint64_t now_us;
    rng_t    rng;
    uint8_t  phy_rate;
    uint32_t loss_pct_x100;     /* per receiver per frame, in 1/100 % */
    uint64_t busy_until_us;
    vair_t   air[16];
    unsigned n_air;
    uint32_t next_tx_id;
    vevent_t *heap;
    size_t   heap_len, heap_cap;
    /* statistics */
    uint32_t frames_sent;
    uint32_t collisions;
    uint32_t losses;
    bool     verbose;
    bool     trace;             /* report tick lag episodes and resyncs */
    uint32_t raster_limit;      /* print at most this many spike events */
    uint32_t raster_printed;
};

void vnet_init(vnet_t *net, uint32_t seed);
void vnet_free(vnet_t *net);

/* Adds a powered-off node. Returns its index or -1. */
int vnet_add(vnet_t *net, uint8_t chip, uint32_t free_heap, uint32_t psram, const node_config_t *cfg,
             int32_t clock_ppm, int64_t clock_offset_us);

/* Powers a node on (init + start at the current time) or off. */
void vnet_power(vnet_t *net, int idx, bool on);

/* Runs the simulation until the given time. */
void vnet_run_until(vnet_t *net, uint64_t until_us);

/* Local clock of a node at the current simulation time. */
uint64_t vnet_local_time(const vnode_t *v, uint64_t sim_us);

/* Index of the node that coordinates, if every powered joined node agrees; -1 otherwise. */
int vnet_coordinator(const vnet_t *net);

/* True when every powered node is a member or coordinator of one coordinator. */
bool vnet_converged(const vnet_t *net);

/* Sync error of node idx against the coordinator's clock, in microseconds (INT32_MIN if unknown). */
int32_t vnet_sync_error_us(const vnet_t *net, int idx);

#endif /* DBRAIN_VNET_H */
