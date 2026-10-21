/*
 * Control plane: unicast messages with application-level acknowledgement.
 *
 * Everything that must arrive (join, assignment, probes, artifact transfer)
 * travels here; everything that must arrive on time (spikes) does not.
 *
 * Frame layout (docs/protocol.md section 3.4):
 *
 *   0   preamble (2)
 *   2   u16 cluster id
 *   4   u16 source node id      (0 before a node has one)
 *   6   u16 destination node id (0 when addressed by MAC only)
 *   8   u8  message type
 *   9   u8  flags: CTRL_FLAG_ACK marks an acknowledgement of `seq`,
 *               CTRL_FLAG_REQ_ACK asks the receiver to acknowledge
 *   10  u8  sequence number (per sender, wraps)
 *   11  u8  payload length
 *   12  payload
 *
 * A reliable message is retransmitted every CTRL_RETRY_MS until its
 * acknowledgement arrives or CTRL_MAX_TRIES is exhausted. Handlers must be
 * idempotent: a lost acknowledgement makes the sender repeat the request.
 */
#ifndef DBRAIN_CONTROL_H
#define DBRAIN_CONTROL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define CTRL_HDR_LEN      12u
#define CTRL_MAX_PAYLOAD  (250u - CTRL_HDR_LEN)
#define CTRL_FLAG_ACK     0x01u
#define CTRL_FLAG_REQ_ACK 0x02u
#define CTRL_RETRY_MS     100u
#define CTRL_MAX_TRIES    31u
#ifndef CTRL_INFLIGHT
#define CTRL_INFLIGHT     4
#endif
#define CTRL_DEDUPE       8

enum {
    CTRL_JOIN_REQ      = 1,  /* cap(16) mac(6) held_shard u16 n_neurons u16          */
    CTRL_JOIN_ACK      = 2,  /* node_id u16 epoch u16 coord_id u16 mode u8 pad u8 recipe(16) */
    CTRL_PING          = 3,  /* empty                                                 */
    CTRL_PONG          = 4,  /* empty                                                 */
    CTRL_PROBE_REQ     = 5,  /* mac(6) of the suspect                                 */
    CTRL_PROBE_RSP     = 6,  /* mac(6) alive u8                                       */
    CTRL_ASSIGN        = 7,  /* shard_id u16 shard_version u16 source u8 pad u8       */
    CTRL_FENCE         = 8,  /* action u8 pad u8 at_tick u32                          */
    CTRL_TELEMETRY_REQ = 9,  /* empty                                                 */
    CTRL_TELEMETRY_RSP = 10, /* telemetry_t as little-endian u32 array                */
    CTRL_ART_BEGIN     = 11, /* shard_id u16 shard_version u16 size u32 crc u32       */
    CTRL_ART_CHUNK     = 12, /* offset u32 data[]                                     */
    CTRL_ART_END       = 13, /* crc u32                                               */
    CTRL_ART_STATUS    = 14, /* status u8 (0 ok, 1 busy, 2 crc, 3 nospace, 4 reject)  */
    CTRL_INJECT        = 15  /* neuron u16 pad u16 current i32 at_tick u32 (0 = now)  */
};

typedef struct {
    uint16_t cluster_id;
    uint16_t src_node;
    uint16_t dst_node;
    uint8_t  type;
    uint8_t  flags;
    uint8_t  seq;
    uint8_t  len;
    const uint8_t *payload;
} ctrl_view_t;

size_t ctrl_encode(uint8_t *out, size_t cap, const ctrl_view_t *v);
bool   ctrl_decode(const uint8_t *in, size_t len, ctrl_view_t *v);

typedef struct {
    uint8_t  mac[6];
    uint8_t  used;
    uint8_t  tries;
    uint8_t  seq;
    uint8_t  type;
    uint8_t  len;
    uint32_t next_ms;
    uint8_t  frame[250];
} ctrl_inflight_t;

typedef struct ctrl ctrl_t;

/* Platform hooks. send returns 0 when the frame was handed to the radio. */
typedef int  (*ctrl_send_fn)(void *ctx, const uint8_t mac[6], const uint8_t *frame, size_t len);
typedef void (*ctrl_fail_fn)(void *ctx, const uint8_t mac[6], uint8_t type, uint8_t seq);
typedef void (*ctrl_acked_fn)(void *ctx, const uint8_t mac[6], uint8_t type, uint8_t seq);

struct ctrl {
    uint16_t cluster_id;
    uint8_t  next_seq;
    ctrl_inflight_t inflight[CTRL_INFLIGHT];
    struct { uint8_t mac[6]; uint8_t seq; uint8_t used; } seen[CTRL_DEDUPE];
    uint8_t  seen_next;
    uint32_t retries;
    uint32_t failures;
    void *ctx;
    ctrl_send_fn send;
    ctrl_fail_fn on_fail;
    ctrl_acked_fn on_acked;
};

void ctrl_init(ctrl_t *c, uint16_t cluster_id, void *ctx, ctrl_send_fn send, ctrl_fail_fn on_fail,
               ctrl_acked_fn on_acked);

/*
 * Sends a message. Reliable messages get CTRL_FLAG_REQ_ACK and an in-flight
 * slot; returns the sequence number used, or -1 when no slot is free or the
 * payload is too long.
 */
int ctrl_send(ctrl_t *c, const uint8_t mac[6], uint16_t src, uint16_t dst, uint8_t type,
              const void *payload, uint8_t len, bool reliable, uint32_t now_ms);

/* Retransmits due messages; reports exhausted ones through on_fail. */
void ctrl_poll(ctrl_t *c, uint32_t now_ms);

/*
 * Feeds a decoded control frame. Acknowledgements settle in-flight entries
 * and return false (nothing for the caller to handle). Requests that ask for
 * an acknowledgement are acknowledged here. Returns true when the caller
 * should handle the message, false for acks and duplicates.
 */
bool ctrl_on_frame(ctrl_t *c, const uint8_t src_mac[6], const ctrl_view_t *v, uint16_t own_id);

/* Number of in-flight reliable messages. */
unsigned ctrl_pending(const ctrl_t *c);

/* Drops in-flight messages to a peer (it died or we left). */
void ctrl_cancel(ctrl_t *c, const uint8_t mac[6]);

#endif /* DBRAIN_CONTROL_H */
