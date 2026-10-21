/*
 * The node agent: everything a board does, independent of the board.
 *
 * node_t owns the membership table, the election, the clock, the control
 * lane and the shard. The platform (firmware port or host simulator) feeds
 * it time, frames and a few callbacks; the agent never calls an OS. The
 * same object therefore runs on every chip and in the cluster simulator,
 * which is how its behaviour is tested without a radio.
 *
 * Lifecycle: node_init() -> node_start() -> node_poll() frequently (every
 * millisecond or faster) and node_on_frame() for every received frame.
 * node_on_sent() reports when a frame left the air; the beacon needs it.
 *
 * States (docs/protocol.md section 4):
 *   DISCOVER     listening for a cluster; heartbeats with node id 0
 *   JOINING      join request in flight to the coordinator
 *   MEMBER       has a node id, follows the coordinator's beacons
 *   COORDINATOR  beacons time, admits joiners, numbers the epoch
 */
#ifndef DBRAIN_NODE_H
#define DBRAIN_NODE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dbrain/membership.h"
#include "dbrain/control.h"
#include "dbrain/timesync.h"
#include "dbrain/slot.h"
#include "dbrain/shard.h"
#include "dbrain/recipe.h"
#include "dbrain/telemetry.h"

enum { NODE_MODE_RECIPE = 0, NODE_MODE_ARTIFACT = 1 };

enum { NODE_LOG_ERROR = 0, NODE_LOG_WARN = 1, NODE_LOG_INFO = 2, NODE_LOG_DEBUG = 3 };

enum {
    NODE_EV_STATE = 1,        /* value = new NODE_STATE_*                  */
    NODE_EV_COORDINATOR = 2,  /* node_id = coordinator, value = epoch       */
    NODE_EV_MEMBER = 3,       /* node_id = member, value = MEMBER_* liveness */
    NODE_EV_SHARD = 4,        /* shard_id, value = neuron count (0 = unbound) */
    NODE_EV_SPIKES = 5,       /* tick, shard_id, count, events              */
    NODE_EV_SYNC = 6,         /* value = residual in microseconds           */
    NODE_EV_TICK_RESYNC = 7,  /* tick = new tick, value = ticks skipped     */
    NODE_EV_JOINED = 8,       /* node_id = our id, value = epoch            */
    NODE_EV_ARTIFACT = 9      /* shard_id, value = status                   */
};

typedef struct {
    uint8_t  type;
    uint16_t node_id;
    uint16_t shard_id;
    uint32_t tick;
    uint16_t count;
    const uint16_t *events;
    int32_t  value;
} node_event_t;

typedef struct {
    void *ctx;
    uint64_t (*now_us)(void *ctx);
    /* mac == NULL broadcasts. tag is the frame type, echoed by node_on_sent. */
    int  (*send)(void *ctx, const uint8_t *mac, const uint8_t *frame, size_t len, uint8_t tag);
    void (*get_mac)(void *ctx, uint8_t mac[6]);
    void (*get_cap)(void *ctx, cap_t *cap);   /* refreshes free_heap; chip fields stay */
    uint32_t (*random)(void *ctx);
    void (*log)(void *ctx, int level, const char *msg);
    void (*event)(void *ctx, const node_event_t *ev);
} node_platform_t;

typedef struct {
    uint16_t cluster_id;
    uint8_t  mode;             /* NODE_MODE_* */
    uint8_t  phy_rate;         /* PHY_RATE_* wanted for an all-ESP32 cluster */
    uint16_t slot_events;
    uint32_t min_period_us;    /* lower bound on the tick period */
    uint16_t tx_latency_us;    /* beacon send-callback latency, see docs/protocol.md 6.2 */
    uint32_t discover_ms;      /* listen this long before forming a cluster */
    recipe_t recipe;           /* recipe this node proposes when it forms a cluster */
    const void *artifact;      /* NODE_MODE_ARTIFACT: image to run */
    uint32_t artifact_size;
    void   *arena;             /* shard working memory */
    size_t  arena_size;
    uint8_t *image_buf;        /* RAM for generated or transferred images */
    uint32_t image_cap;
} node_config_t;

#define NODE_DISCOVER_MS_DEFAULT  5000u
#define NODE_JOIN_TRIES           5u
#define NODE_SLEW_US_PER_MS       10u
#define NODE_MAX_CATCHUP_TICKS    50u
#define NODE_REBUILD_HOLDOFF_MS   2000u
#define NODE_PROMOTE_STEP_MS      300u

typedef struct {
    node_platform_t pf;
    node_config_t   cfg;
    uint8_t  mac[6];
    cap_t    cap;
    uint8_t  state;
    uint16_t node_id;
    uint16_t epoch;

    /* the coordinator we follow (or ourselves) */
    bool     have_coord;
    uint16_t coord_id;
    uint8_t  coord_mac[6];
    uint32_t coord_score;
    uint32_t coord_last_ms;

    /* cluster parameters, authoritative from the beacon */
    bool     cluster_valid;
    uint16_t cluster_members;
    uint64_t tick_origin_us;
    uint32_t tick_period_us;
    uint16_t slot_len_us;
    uint16_t slot_events;
    uint8_t  phy_rate;
    uint32_t slot_version;
    uint8_t  cluster_flags;

    membership_t members;
    ctrl_t       ctrl;
    timesync_t   sync;
    telemetry_t  tm;

    /* beacon pairing on the receive side */
    bool     have_prev_beacon;
    uint16_t prev_beacon_seq;
    uint64_t prev_beacon_rx_us;

    /* coordinator side */
    uint16_t beacon_seq;
    uint16_t beacon_tx_seq;    /* beacon whose transmit time is known */
    uint64_t beacon_tx_us;     /* when beacon beacon_tx_seq left the air, 0 if unknown */
    bool     beacon_inflight;  /* a beacon awaits its send callback */
    uint32_t next_beacon_ms;
    uint32_t roster_version_planned;

    /* shard */
    shard_t  shard;
    bool     shard_valid;
    uint16_t shard_version;
    recipe_t recipe;
    bool     recipe_valid;
    uint16_t recipe_neurons;
    uint32_t roster_version_built;
    uint32_t rebuild_after_ms;
    bool     rebuild_wanted;
    bool     paused;
    uint32_t resume_tick;

    /* tick and transmit */
    uint32_t last_integrated;   /* last tick integrated by this node */
    bool     tick_started;
    bool     bundle_pending;
    uint32_t bundle_tick;
    uint16_t bundle_seq;
    uint32_t last_slew_ms;

    /* timers */
    uint32_t next_hb_ms;
    uint32_t discover_until_ms;
    uint32_t join_next_ms;
    uint8_t  join_tries;
    uint32_t promote_at_ms;
    bool     promote_scheduled;
    bool     have_tick_carry;   /* tick numbering to continue if we take over */
    uint32_t tick_carry;
    uint32_t tick_carry_ms;     /* local time at which tick_carry was current */
    uint16_t hb_seq;

    /* artifact transfer */
    bool     art_busy;
    uint32_t art_size;
    uint32_t art_crc;
    uint16_t art_shard;
    uint16_t art_version;

    uint8_t  txbuf[250];
} node_t;

void node_config_default(node_config_t *c);

/* Binds platform and config; does not transmit. Returns 0 or a negative error. */
int  node_init(node_t *n, const node_platform_t *pf, const node_config_t *cfg);

/* Enters DISCOVER. */
void node_start(node_t *n);

/* Drives timers, the tick loop and the control lane. Call often. */
void node_poll(node_t *n);

/*
 * A received frame. rx_us is the receive timestamp on the local clock;
 * hw_ts says whether it came from the radio (true) or from software. */
void node_on_frame(node_t *n, const uint8_t src_mac[6], const uint8_t *data, size_t len,
                   uint64_t rx_us, bool hw_ts);

/* A frame with the given tag finished transmitting at local time tx_us. */
void node_on_sent(node_t *n, uint8_t tag, uint64_t tx_us);

/* Cluster time estimate; false while unsynchronised. */
bool node_cluster_time(const node_t *n, uint64_t local_us, uint64_t *cluster_us);

/* Current cluster tick as this node sees it; false while unsynchronised. */
bool node_current_tick(const node_t *n, uint32_t *tick);

/* Drives an external current into a local neuron at a tick (0 = next). */
int node_inject(node_t *n, uint16_t neuron, accum_t current, uint32_t at_tick);

const char *node_state_name(uint8_t state);

#endif /* DBRAIN_NODE_H */
