#include <string.h>
#include "dbrain/control.h"
#include "dbrain/frame.h"

size_t ctrl_encode(uint8_t *out, size_t cap, const ctrl_view_t *v)
{
    size_t len = CTRL_HDR_LEN + v->len;

    if (v->len > CTRL_MAX_PAYLOAD || cap < len) return 0;
    out[0] = FRAME_MAGIC;
    out[1] = frame_ver_type(FRAME_CONTROL);
    wr16(out + 2, v->cluster_id);
    wr16(out + 4, v->src_node);
    wr16(out + 6, v->dst_node);
    out[8] = v->type;
    out[9] = v->flags;
    out[10] = v->seq;
    out[11] = v->len;
    if (v->len) memcpy(out + CTRL_HDR_LEN, v->payload, v->len);
    return len;
}

bool ctrl_decode(const uint8_t *in, size_t len, ctrl_view_t *v)
{
    if (frame_type(in, len) != FRAME_CONTROL || len < CTRL_HDR_LEN) return false;
    v->cluster_id = rd16(in + 2);
    v->src_node = rd16(in + 4);
    v->dst_node = rd16(in + 6);
    v->type = in[8];
    v->flags = in[9];
    v->seq = in[10];
    v->len = in[11];
    v->payload = in + CTRL_HDR_LEN;
    return len == CTRL_HDR_LEN + v->len;
}

void ctrl_init(ctrl_t *c, uint16_t cluster_id, void *ctx, ctrl_send_fn send, ctrl_fail_fn on_fail,
               ctrl_acked_fn on_acked)
{
    memset(c, 0, sizeof(*c));
    c->cluster_id = cluster_id;
    c->ctx = ctx;
    c->send = send;
    c->on_fail = on_fail;
    c->on_acked = on_acked;
}

int ctrl_send(ctrl_t *c, const uint8_t mac[6], uint16_t src, uint16_t dst, uint8_t type,
              const void *payload, uint8_t len, bool reliable, uint32_t now_ms)
{
    ctrl_view_t v;
    uint8_t frame[250];
    size_t flen;
    uint8_t seq = c->next_seq++;

    v.cluster_id = c->cluster_id;
    v.src_node = src;
    v.dst_node = dst;
    v.type = type;
    v.flags = reliable ? CTRL_FLAG_REQ_ACK : 0;
    v.seq = seq;
    v.len = len;
    v.payload = (const uint8_t *)payload;
    flen = ctrl_encode(frame, sizeof frame, &v);
    if (flen == 0) return -1;

    if (reliable) {
        unsigned i;
        ctrl_inflight_t *slot = NULL;
        for (i = 0; i < CTRL_INFLIGHT; i++)
            if (!c->inflight[i].used) { slot = &c->inflight[i]; break; }
        if (slot == NULL) return -1;
        memcpy(slot->mac, mac, 6);
        slot->used = 1;
        slot->tries = 1;
        slot->seq = seq;
        slot->type = type;
        slot->len = (uint8_t)flen;
        slot->next_ms = now_ms + CTRL_RETRY_MS;
        memcpy(slot->frame, frame, flen);
    }
    c->send(c->ctx, mac, frame, flen);
    return seq;
}

void ctrl_poll(ctrl_t *c, uint32_t now_ms)
{
    unsigned i;

    for (i = 0; i < CTRL_INFLIGHT; i++) {
        ctrl_inflight_t *s = &c->inflight[i];
        if (!s->used || (int32_t)(now_ms - s->next_ms) < 0) continue;
        if (s->tries >= CTRL_MAX_TRIES) {
            uint8_t mac[6], type = s->type, seq = s->seq;
            memcpy(mac, s->mac, 6);
            s->used = 0;
            c->failures++;
            if (c->on_fail) c->on_fail(c->ctx, mac, type, seq);
            continue;
        }
        s->tries++;
        s->next_ms = now_ms + CTRL_RETRY_MS;
        c->retries++;
        c->send(c->ctx, s->mac, s->frame, s->len);
    }
}

static void send_ack(ctrl_t *c, const uint8_t mac[6], const ctrl_view_t *req, uint16_t own_id)
{
    ctrl_view_t a;
    uint8_t frame[CTRL_HDR_LEN];
    size_t len;

    a.cluster_id = c->cluster_id;
    a.src_node = own_id;
    a.dst_node = req->src_node;
    a.type = req->type;
    a.flags = CTRL_FLAG_ACK;
    a.seq = req->seq;
    a.len = 0;
    a.payload = NULL;
    len = ctrl_encode(frame, sizeof frame, &a);
    c->send(c->ctx, mac, frame, len);
}

static bool seen_before(ctrl_t *c, const uint8_t mac[6], uint8_t seq)
{
    unsigned i;
    for (i = 0; i < CTRL_DEDUPE; i++)
        if (c->seen[i].used && c->seen[i].seq == seq && memcmp(c->seen[i].mac, mac, 6) == 0)
            return true;
    memcpy(c->seen[c->seen_next].mac, mac, 6);
    c->seen[c->seen_next].seq = seq;
    c->seen[c->seen_next].used = 1;
    c->seen_next = (uint8_t)((c->seen_next + 1) % CTRL_DEDUPE);
    return false;
}

bool ctrl_on_frame(ctrl_t *c, const uint8_t src_mac[6], const ctrl_view_t *v, uint16_t own_id)
{
    if (v->cluster_id != c->cluster_id) return false;
    if (v->flags & CTRL_FLAG_ACK) {
        unsigned i;
        for (i = 0; i < CTRL_INFLIGHT; i++) {
            ctrl_inflight_t *s = &c->inflight[i];
            if (s->used && s->seq == v->seq && memcmp(s->mac, src_mac, 6) == 0) {
                s->used = 0;
                if (c->on_acked) c->on_acked(c->ctx, src_mac, s->type, s->seq);
                break;
            }
        }
        return false;
    }
    if (v->flags & CTRL_FLAG_REQ_ACK) {
        send_ack(c, src_mac, v, own_id);
        if (seen_before(c, src_mac, v->seq)) return false;
    }
    return true;
}

unsigned ctrl_pending(const ctrl_t *c)
{
    unsigned i, n = 0;
    for (i = 0; i < CTRL_INFLIGHT; i++)
        if (c->inflight[i].used) n++;
    return n;
}

void ctrl_cancel(ctrl_t *c, const uint8_t mac[6])
{
    unsigned i;
    for (i = 0; i < CTRL_INFLIGHT; i++)
        if (c->inflight[i].used && memcmp(c->inflight[i].mac, mac, 6) == 0) c->inflight[i].used = 0;
}
