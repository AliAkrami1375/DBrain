#include "dbrain/bundle.h"
#include "dbrain/frame.h"

size_t bundle_encode(uint8_t *out, size_t cap, uint16_t src_node, uint32_t tick, uint16_t seq,
                     uint8_t flags, const uint16_t *events, uint8_t count)
{
    size_t len = BUNDLE_HDR_LEN + 2u * count;
    uint8_t i;

    if (count > BUNDLE_MAX_EVENTS || cap < len) return 0;
    out[0] = FRAME_MAGIC;
    out[1] = frame_ver_type(FRAME_BUNDLE);
    wr16(out + 2, src_node);
    wr32(out + 4, tick);
    wr16(out + 8, seq);
    out[10] = count;
    out[11] = flags;
    for (i = 0; i < count; i++) wr16(out + BUNDLE_HDR_LEN + 2u * i, events[i]);
    return len;
}

bool bundle_decode(const uint8_t *buf, size_t len, bundle_view_t *v)
{
    if (frame_type(buf, len) != FRAME_BUNDLE) return false;
    if (len < BUNDLE_HDR_LEN) return false;
    v->src_node = rd16(buf + 2);
    v->tick = rd32(buf + 4);
    v->seq = rd16(buf + 8);
    v->count = buf[10];
    v->flags = buf[11];
    v->events = buf + BUNDLE_HDR_LEN;
    if (v->count > BUNDLE_MAX_EVENTS) return false;
    if (len != BUNDLE_HDR_LEN + 2u * v->count) return false;
    return true;
}

uint16_t seq_tracker_observe(seq_tracker_t *t, uint16_t seq)
{
    uint16_t gap;

    if (!t->seen) {
        t->seen = true;
        t->last_seq = seq;
        return 0;
    }
    gap = (uint16_t)(seq - t->last_seq);
    t->last_seq = seq;
    if (gap == 0) return 0;              /* duplicate: cannot happen on one channel, ignore */
    if (gap > 0x8000u) return 0;         /* backwards jump: source restarted */
    return (uint16_t)(gap - 1u);
}
