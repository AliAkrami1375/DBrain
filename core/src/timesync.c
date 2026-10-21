#include <string.h>
#include "dbrain/timesync.h"
#include "dbrain/frame.h"

void timesync_init(timesync_t *s)
{
    memset(s, 0, sizeof(*s));
}

void timesync_reset(timesync_t *s)
{
    s->n = 0;
    s->head = 0;
    s->last_residual_us = 0;
}

static int64_t i64abs(int64_t v)
{
    return v < 0 ? -v : v;
}

/* Oldest sample index. */
static unsigned first_index(const timesync_t *s)
{
    return (unsigned)(s->head + SYNC_SAMPLES - s->n) % SYNC_SAMPLES;
}

/*
 * Least squares over the samples, relative to the oldest sample so that
 * every intermediate fits in 64 bits (x spans at most SYNC_MAX_SPAN_US,
 * |y| at most SYNC_MAX_SAMPLE_US, n at most 8).
 */
bool timesync_estimate(const timesync_t *s, uint64_t local_us, int64_t *offset_out)
{
    unsigned i, f;
    int64_t x0, y0, sx = 0, sy = 0, sxx = 0, sxy = 0, n, dxx, dxy, slope_q32 = 0, xq, est;

    if (s->n == 0) return false;
    f = first_index(s);
    x0 = (int64_t)s->local[f];
    y0 = s->offset[f];
    n = s->n;
    for (i = 0; i < s->n; i++) {
        unsigned k = (f + i) % SYNC_SAMPLES;
        int64_t x = (int64_t)s->local[k] - x0;
        int64_t y = s->offset[k] - y0;
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
    }
    dxx = n * sxx - sx * sx;
    dxy = n * sxy - sx * sy;
    if (dxx > 0) {
        /* slope = dxy / dxx in Q32 without overflowing the numerator */
        int shift = 32;
        int64_t num = dxy;
        while (shift > 0 && i64abs(num) > (INT64_MAX >> shift)) shift--;
        slope_q32 = ((num * ((int64_t)1 << shift)) / dxx) * ((int64_t)1 << (32 - shift));
        if (slope_q32 > SYNC_MAX_SLOPE_Q32) slope_q32 = SYNC_MAX_SLOPE_Q32;
        if (slope_q32 < -SYNC_MAX_SLOPE_Q32) slope_q32 = -SYNC_MAX_SLOPE_Q32;
    }
    xq = (int64_t)local_us - x0;
    /* est = mean_y + slope * (x - mean_x), evaluated as one division */
    est = (sy * ((int64_t)1 << 32) + slope_q32 * (n * xq - sx)) / (n * ((int64_t)1 << 32));
    *offset_out = y0 + est;
    return true;
}

int32_t timesync_add(timesync_t *s, uint64_t local_us, uint64_t coord_us)
{
    int64_t offset = (int64_t)coord_us - (int64_t)local_us;
    int64_t est = 0, residual = 0;

    if (s->n > 0) {
        unsigned f = first_index(s);
        unsigned last = (unsigned)(s->head + SYNC_SAMPLES - 1) % SYNC_SAMPLES;
        /* time must move forward and the window must stay bounded */
        if (local_us <= s->local[last]) timesync_reset(s);
        else if ((int64_t)(local_us - s->local[f]) > SYNC_MAX_SPAN_US) {
            /* drop oldest samples until the span fits */
            while (s->n > 1 && (int64_t)(local_us - s->local[first_index(s)]) > SYNC_MAX_SPAN_US) s->n--;
            if ((int64_t)(local_us - s->local[first_index(s)]) > SYNC_MAX_SPAN_US) timesync_reset(s);
        }
    }
    if (s->n > 0) {
        timesync_estimate(s, local_us, &est);
        residual = est - offset;
        if (i64abs(residual) > SYNC_MAX_SAMPLE_US) {
            timesync_reset(s);
            residual = 0;
        }
    }
    s->local[s->head] = local_us;
    s->offset[s->head] = offset;
    s->head = (uint8_t)((s->head + 1) % SYNC_SAMPLES);
    if (s->n < SYNC_SAMPLES) s->n++;
    s->samples_total++;
    if (residual > INT32_MAX) residual = INT32_MAX;
    if (residual < INT32_MIN) residual = INT32_MIN;
    s->last_residual_us = (int32_t)residual;
    return s->last_residual_us;
}

bool timesync_slew(timesync_t *s, uint64_t local_us, int64_t max_step_us, int64_t step_threshold_us)
{
    int64_t target, err;

    if (!timesync_estimate(s, local_us, &target)) return false;
    if (!s->applied_valid) {
        s->applied_offset = target;
        s->applied_valid = true;
        return true;
    }
    err = target - s->applied_offset;
    if (i64abs(err) > step_threshold_us) {
        s->applied_offset = target;
        return true;
    }
    if (err > max_step_us) err = max_step_us;
    if (err < -max_step_us) err = -max_step_us;
    s->applied_offset += err;
    return false;
}

bool timesync_cluster_time(const timesync_t *s, uint64_t local_us, uint64_t *cluster_us)
{
    if (!s->applied_valid) return false;
    *cluster_us = (uint64_t)((int64_t)local_us + s->applied_offset);
    return true;
}

/* ---- beacon ----------------------------------------------------------- */

size_t beacon_encode(uint8_t *out, size_t cap, const beacon_t *b)
{
    if (cap < BEACON_LEN) return 0;
    out[0] = FRAME_MAGIC;
    out[1] = frame_ver_type(FRAME_BEACON);
    wr16(out + 2, b->cluster_id);
    wr16(out + 4, b->epoch);
    wr16(out + 6, b->coord_id);
    wr_mac(out + 8, b->coord_mac);
    wr16(out + 14, b->seq);
    wr16(out + 16, b->tx_latency_us);
    wr16(out + 18, b->member_count);
    wr64(out + 20, b->prev_tx_time_us);
    wr64(out + 28, b->tick_origin_us);
    wr32(out + 36, b->tick_period_us);
    wr32(out + 40, b->tick_now);
    wr32(out + 44, b->slot_version);
    wr16(out + 48, b->slot_len_us);
    wr16(out + 50, b->slot_events);
    out[52] = b->phy_rate;
    out[53] = b->flags;
    wr16(out + 54, b->prev_tx_seq);
    wr32(out + 56, b->coord_score);
    return BEACON_LEN;
}

bool beacon_decode(const uint8_t *in, size_t len, beacon_t *b)
{
    if (frame_type(in, len) != FRAME_BEACON || len != BEACON_LEN) return false;
    b->cluster_id = rd16(in + 2);
    b->epoch = rd16(in + 4);
    b->coord_id = rd16(in + 6);
    rd_mac(in + 8, b->coord_mac);
    b->seq = rd16(in + 14);
    b->tx_latency_us = rd16(in + 16);
    b->member_count = rd16(in + 18);
    b->prev_tx_time_us = rd64(in + 20);
    b->tick_origin_us = rd64(in + 28);
    b->tick_period_us = rd32(in + 36);
    b->tick_now = rd32(in + 40);
    b->slot_version = rd32(in + 44);
    b->slot_len_us = rd16(in + 48);
    b->slot_events = rd16(in + 50);
    b->phy_rate = in[52];
    b->flags = in[53];
    b->prev_tx_seq = rd16(in + 54);
    b->coord_score = rd32(in + 56);
    if (b->tick_period_us == 0 || b->epoch == 0) return false;
    return true;
}
