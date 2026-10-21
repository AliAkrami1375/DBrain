#include "unit.h"
#include "dbrain/control.h"
#include "dbrain/frame.h"

static uint8_t last_frame[250];
static size_t last_len;
static int sends;
static int fails, acked;
static uint8_t fail_type;

static int fake_send(void *ctx, const uint8_t mac[6], const uint8_t *frame, size_t len)
{
    (void)ctx; (void)mac;
    memcpy(last_frame, frame, len);
    last_len = len;
    sends++;
    return 0;
}

static void fake_fail(void *ctx, const uint8_t mac[6], uint8_t type, uint8_t seq)
{
    (void)ctx; (void)mac; (void)seq;
    fails++;
    fail_type = type;
}

static void fake_acked(void *ctx, const uint8_t mac[6], uint8_t type, uint8_t seq)
{
    (void)ctx; (void)mac; (void)type; (void)seq;
    acked++;
}

int main(void)
{
    ctrl_t a, b;
    ctrl_view_t v, w;
    uint8_t mac_a[6] = {1,1,1,1,1,1}, mac_b[6] = {2,2,2,2,2,2};
    uint8_t payload[4] = {9, 8, 7, 6};
    uint8_t buf[250], big[CTRL_MAX_PAYLOAD + 1];
    uint8_t ack_frame[250];
    size_t ack_len, len;
    int seq;
    uint32_t now = 1000;
    unsigned i;

    /* encode / decode */
    v.cluster_id = 42; v.src_node = 3; v.dst_node = 1; v.type = CTRL_PING; v.flags = CTRL_FLAG_REQ_ACK;
    v.seq = 77; v.len = 4; v.payload = payload;
    len = ctrl_encode(buf, sizeof buf, &v);
    CHECK_EQ(len, 16);
    CHECK_EQ(frame_type(buf, len), FRAME_CONTROL);
    CHECK(ctrl_decode(buf, len, &w));
    CHECK_EQ(w.cluster_id, 42);
    CHECK_EQ(w.src_node, 3);
    CHECK_EQ(w.dst_node, 1);
    CHECK_EQ(w.type, CTRL_PING);
    CHECK_EQ(w.flags, CTRL_FLAG_REQ_ACK);
    CHECK_EQ(w.seq, 77);
    CHECK_EQ(w.len, 4);
    CHECK_EQ(w.payload[3], 6);
    CHECK(!ctrl_decode(buf, len - 1, &w));
    CHECK(!ctrl_decode(buf, len + 1, &w));
    v.len = (uint8_t)sizeof big; v.payload = big;
    CHECK_EQ(ctrl_encode(buf, sizeof buf, &v), 0);
    v.len = CTRL_MAX_PAYLOAD;
    CHECK_EQ(ctrl_encode(buf, sizeof buf, &v), 250);

    /* reliable delivery: a sends to b, b acknowledges, a settles */
    ctrl_init(&a, 42, NULL, fake_send, fake_fail, fake_acked);
    ctrl_init(&b, 42, NULL, fake_send, fake_fail, fake_acked);
    sends = 0;
    seq = ctrl_send(&a, mac_b, 1, 2, CTRL_PING, payload, 4, true, now);
    CHECK(seq >= 0);
    CHECK_EQ(sends, 1);
    CHECK_EQ(ctrl_pending(&a), 1);
    CHECK(ctrl_decode(last_frame, last_len, &w));
    CHECK(ctrl_on_frame(&b, mac_a, &w, 2));         /* b handles it and acks */
    CHECK_EQ(sends, 2);
    CHECK(ctrl_decode(last_frame, last_len, &w));
    CHECK_EQ(w.flags, CTRL_FLAG_ACK);
    CHECK_EQ(w.seq, seq);
    CHECK_EQ(w.type, CTRL_PING);
    CHECK_EQ(w.src_node, 2);
    memcpy(ack_frame, last_frame, last_len);
    ack_len = last_len;
    CHECK(!ctrl_on_frame(&a, mac_b, &w, 1));        /* an ack is consumed */
    CHECK_EQ(ctrl_pending(&a), 0);
    CHECK_EQ(acked, 1);
    ctrl_poll(&a, now + 1000);
    CHECK_EQ(sends, 2);                             /* nothing left to retry */

    /* a duplicate request (ack lost) is acked again but not handled twice */
    seq = ctrl_send(&a, mac_b, 1, 2, CTRL_PING, payload, 4, true, now);
    CHECK(ctrl_decode(last_frame, last_len, &w));
    memcpy(buf, last_frame, last_len);
    len = last_len;
    CHECK(ctrl_on_frame(&b, mac_a, &w, 2));
    CHECK(ctrl_decode(buf, len, &w));
    CHECK(!ctrl_on_frame(&b, mac_a, &w, 2));
    ctrl_cancel(&a, mac_b);
    CHECK_EQ(ctrl_pending(&a), 0);

    /* retries every 100 ms, gives up after 31 tries */
    sends = 0;
    seq = ctrl_send(&a, mac_b, 1, 2, CTRL_ASSIGN, payload, 4, true, now);
    for (i = 1; i <= 40; i++) ctrl_poll(&a, now + i * 100);
    CHECK_EQ(sends, CTRL_MAX_TRIES);
    CHECK_EQ(fails, 1);
    CHECK_EQ(fail_type, CTRL_ASSIGN);
    CHECK_EQ(ctrl_pending(&a), 0);
    CHECK_EQ(a.retries, CTRL_MAX_TRIES - 1);

    /* an ack for the wrong peer or seq changes nothing */
    sends = 0;
    seq = ctrl_send(&a, mac_b, 1, 2, CTRL_PING, NULL, 0, true, now);
    CHECK(ctrl_decode(ack_frame, ack_len, &w));
    w.seq = (uint8_t)(seq + 1);
    CHECK(!ctrl_on_frame(&a, mac_b, &w, 1));
    CHECK_EQ(ctrl_pending(&a), 1);
    w.seq = (uint8_t)seq;
    CHECK(!ctrl_on_frame(&a, mac_a, &w, 1));
    CHECK_EQ(ctrl_pending(&a), 1);
    CHECK(!ctrl_on_frame(&a, mac_b, &w, 1));
    CHECK_EQ(ctrl_pending(&a), 0);

    /* unreliable sends take no slot; slots are limited */
    CHECK(ctrl_send(&a, mac_b, 1, 2, CTRL_PONG, NULL, 0, false, now) >= 0);
    CHECK_EQ(ctrl_pending(&a), 0);
    for (i = 0; i < CTRL_INFLIGHT; i++) CHECK(ctrl_send(&a, mac_b, 1, 2, CTRL_PING, NULL, 0, true, now) >= 0);
    CHECK_EQ(ctrl_send(&a, mac_b, 1, 2, CTRL_PING, NULL, 0, true, now), -1);
    ctrl_cancel(&a, mac_b);

    /* other clusters are ignored */
    w.cluster_id = 43; w.flags = CTRL_FLAG_REQ_ACK; w.type = CTRL_PING;
    sends = 0;
    CHECK(!ctrl_on_frame(&b, mac_a, &w, 2));
    CHECK_EQ(sends, 0);
    UNIT_END();
}
