/*
 * Clock synchronisation and the tick beacon.
 *
 * A broadcast reaches every receiver within tens of microseconds of each
 * other while the sender's transmit instant wobbles by milliseconds, so
 * synchronisation keys off reception. The coordinator broadcasts a beacon
 * once per BEACON_INTERVAL. A beacon carries the coordinator's local time at
 * which an earlier beacon (named by its sequence number) finished
 * transmitting; a receiver pairs that with its own hardware receive timestamp
 * of that earlier beacon and obtains one sample of (local time, coordinator
 * time). The coordinator never has two beacons in flight, so the transmit
 * time is always attributed to the right one. An 8-point linear regression over the
 * samples estimates offset and crystal skew; the node then slews, never
 * steps, the offset it applies, unless the error exceeds a whole tick.
 *
 * Cluster time is the coordinator's microsecond clock. Tick N starts at
 * cluster time tick_origin_us + N * tick_period_us.
 */
#ifndef DBRAIN_TIMESYNC_H
#define DBRAIN_TIMESYNC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define SYNC_SAMPLES        8
#define SYNC_MAX_SPAN_US    60000000LL   /* samples older than this are discarded */
#define SYNC_MAX_SAMPLE_US  1000000LL    /* an offset jump beyond this resets the filter */
#define SYNC_MAX_SLOPE_Q32  42949673LL   /* |skew| capped at 1% (2^32 * 0.01) */

#define BEACON_INTERVAL_MS  1000u
#define BEACON_TIMEOUT_MS   (3u * BEACON_INTERVAL_MS + 500u)

typedef struct {
    uint64_t local[SYNC_SAMPLES];
    int64_t  offset[SYNC_SAMPLES];   /* coordinator time minus local time */
    uint8_t  n;
    uint8_t  head;                   /* next write position */
    bool     applied_valid;
    int64_t  applied_offset;
    int32_t  last_residual_us;
    uint32_t samples_total;
} timesync_t;

void timesync_init(timesync_t *s);
void timesync_reset(timesync_t *s);

/*
 * Adds one sample. Returns the residual in microseconds: the estimate at
 * local_us before this sample was added minus the observed offset (0 for the
 * first sample). The filter resets itself on an implausible jump.
 */
int32_t timesync_add(timesync_t *s, uint64_t local_us, uint64_t coord_us);

/* Regression estimate of (coordinator - local) at local_us. False without samples. */
bool timesync_estimate(const timesync_t *s, uint64_t local_us, int64_t *offset_out);

/*
 * Moves the applied offset toward the estimate by at most max_step_us. If
 * the error exceeds step_threshold_us (or nothing was applied yet) the
 * offset is set outright. Returns true when it stepped.
 */
bool timesync_slew(timesync_t *s, uint64_t local_us, int64_t max_step_us, int64_t step_threshold_us);

/* Cluster time with the applied offset. False until the first slew. */
bool timesync_cluster_time(const timesync_t *s, uint64_t local_us, uint64_t *cluster_us);

/* ---- beacon frame, 60 bytes (docs/protocol.md section 3.3) ------------ */

#define BEACON_LEN 60u
#define BEACON_FLAG_PACED  0x01u
#define BEACON_FLAG_RECIPE 0x02u

typedef struct {
    uint16_t cluster_id;
    uint16_t epoch;
    uint16_t coord_id;
    uint8_t  coord_mac[6];
    uint16_t seq;
    uint16_t tx_latency_us;     /* subtracted from prev_tx_time_us by receivers */
    uint16_t member_count;
    uint64_t prev_tx_time_us;   /* coordinator time when beacon prev_tx_seq left the air, 0 if unknown */
    uint64_t tick_origin_us;    /* coordinator time of the start of tick 0 */
    uint32_t tick_period_us;
    uint32_t tick_now;          /* coordinator's tick when the beacon was built */
    uint32_t slot_version;
    uint16_t slot_len_us;
    uint16_t slot_events;
    uint8_t  phy_rate;
    uint8_t  flags;
    uint16_t prev_tx_seq;       /* which beacon prev_tx_time_us refers to */
    uint32_t coord_score;       /* cap_score() of the coordinator, for election by beacon */
} beacon_t;

size_t beacon_encode(uint8_t *out, size_t cap, const beacon_t *b);
bool   beacon_decode(const uint8_t *in, size_t len, beacon_t *b);

#endif /* DBRAIN_TIMESYNC_H */
