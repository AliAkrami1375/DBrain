#include "unit.h"
#include "dbrain/bundle.h"
#include "dbrain/frame.h"

int main(void)
{
    uint8_t buf[256];
    uint16_t ev[BUNDLE_MAX_EVENTS + 1];
    bundle_view_t v;
    seq_tracker_t st = { 0, false };
    size_t len;
    unsigned i;

    for (i = 0; i <= BUNDLE_MAX_EVENTS; i++) ev[i] = (uint16_t)(i * 7u + 3u);

    len = bundle_encode(buf, sizeof buf, 0x1234, 0xDEADBEEFu, 0xBEEF, BUNDLE_FLAG_MORE, ev, 3);
    CHECK_EQ(len, 18);
    CHECK_EQ(buf[0], 0xDB);
    CHECK_EQ(buf[1], (DBRAIN_PROTO_VERSION << 4) | FRAME_BUNDLE);
    CHECK_EQ(frame_type(buf, len), FRAME_BUNDLE);
    CHECK(bundle_decode(buf, len, &v));
    CHECK_EQ(v.src_node, 0x1234);
    CHECK_EQ(v.tick, 0xDEADBEEFu);
    CHECK_EQ(v.seq, 0xBEEF);
    CHECK_EQ(v.count, 3);
    CHECK_EQ(v.flags, BUNDLE_FLAG_MORE);
    CHECK_EQ(bundle_event(&v, 0), 3);
    CHECK_EQ(bundle_event(&v, 2), 17);

    /* full frame is exactly the ESP-NOW v1 limit */
    len = bundle_encode(buf, sizeof buf, 1, 2, 3, 0, ev, BUNDLE_MAX_EVENTS);
    CHECK_EQ(len, FRAME_MAX_LEN);
    CHECK(bundle_decode(buf, len, &v));
    CHECK_EQ(bundle_event(&v, 118), 118 * 7 + 3);
    CHECK_EQ(bundle_encode(buf, sizeof buf, 1, 2, 3, 0, ev, BUNDLE_MAX_EVENTS + 1), 0);
    CHECK_EQ(bundle_encode(buf, 17, 1, 2, 3, 0, ev, 3), 0);

    /* malformed frames are rejected */
    len = bundle_encode(buf, sizeof buf, 1, 2, 3, 0, ev, 2);
    CHECK(!bundle_decode(buf, len - 1, &v));
    CHECK(!bundle_decode(buf, len + 1, &v));
    buf[0] = 0xDC;
    CHECK(!bundle_decode(buf, len, &v));
    buf[0] = 0xDB;
    buf[1] = (uint8_t)(((DBRAIN_PROTO_VERSION + 1) << 4) | FRAME_BUNDLE);
    CHECK(!bundle_decode(buf, len, &v));
    buf[1] = frame_ver_type(FRAME_HEARTBEAT);
    CHECK(!bundle_decode(buf, len, &v));
    CHECK(!bundle_decode(buf, 5, &v));

    /* sequence gaps */
    CHECK_EQ(seq_tracker_observe(&st, 100), 0);
    CHECK_EQ(seq_tracker_observe(&st, 101), 0);
    CHECK_EQ(seq_tracker_observe(&st, 104), 2);
    CHECK_EQ(seq_tracker_observe(&st, 104), 0);
    st.last_seq = 0xFFFE;
    CHECK_EQ(seq_tracker_observe(&st, 1), 2);      /* wraps through 0xFFFF and 0 */
    CHECK_EQ(seq_tracker_observe(&st, 0), 0);      /* source restarted */
    CHECK_EQ(seq_tracker_observe(&st, 1), 0);
    UNIT_END();
}
