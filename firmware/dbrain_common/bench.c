#include <stdio.h>
#include <string.h>
#include "bench.h"
#include "dbrain/frame.h"

static bench_send_fn send_fn;
static struct {
    bool     tx, rx;
    uint32_t count, sent, len;
    uint32_t interval_us;
    uint64_t next_us, start_us, end_us;
    uint32_t seq;
    /* rx */
    uint32_t received, gaps, bytes, bad;
    bool     seen;
    uint32_t last_seq;
    uint64_t last_rx_us, first_rx_us;
    uint64_t gap_min, gap_max, gap_sum;
    int32_t  rssi_sum;
} b;

void bench_init(bench_send_fn send)
{
    send_fn = send;
    memset(&b, 0, sizeof b);
}

int bench_start_tx(uint32_t count, uint16_t len, uint32_t interval_us, uint64_t now_us)
{
    if (len < 8 || len > 250 || count == 0) return -1;
    memset(&b, 0, sizeof b);
    b.tx = true;
    b.count = count;
    b.len = len;
    b.interval_us = interval_us;
    b.next_us = now_us;
    b.start_us = now_us;
    return 0;
}

void bench_start_rx(void)
{
    memset(&b, 0, sizeof b);
    b.rx = true;
    b.gap_min = UINT64_MAX;
}

void bench_stop(void)
{
    b.tx = false;
    b.rx = false;
}

bool bench_active(void)
{
    return b.tx || b.rx;
}

void bench_poll(uint64_t now_us)
{
    uint8_t frame[250];

    if (!b.tx || now_us < b.next_us) return;
    if (b.sent >= b.count) {
        b.tx = false;
        b.end_us = now_us;
        bench_report(now_us);
        return;
    }
    memset(frame, 0xA5, sizeof frame);
    frame[0] = FRAME_MAGIC;
    frame[1] = frame_ver_type(FRAME_BENCH);
    wr32(frame + 2, b.seq++);
    wr16(frame + 6, (uint16_t)b.len);
    if (send_fn(NULL, frame, b.len) == 0) b.sent++;
    b.next_us += b.interval_us;
    if (b.next_us < now_us) b.next_us = now_us;
}

void bench_on_frame(const uint8_t *data, size_t len, uint64_t rx_us, int8_t rssi)
{
    uint32_t seq;

    if (!b.rx) return;
    if (len < 8 || rd16(data + 6) != len) { b.bad++; return; }
    seq = rd32(data + 2);
    b.received++;
    b.bytes += (uint32_t)len;
    b.rssi_sum += rssi;
    if (b.seen) {
        uint64_t gap = rx_us - b.last_rx_us;
        if (seq > b.last_seq + 1) b.gaps += seq - b.last_seq - 1;
        if (gap < b.gap_min) b.gap_min = gap;
        if (gap > b.gap_max) b.gap_max = gap;
        b.gap_sum += gap;
    } else {
        b.first_rx_us = rx_us;
    }
    b.seen = true;
    b.last_seq = seq;
    b.last_rx_us = rx_us;
}

void bench_report(uint64_t now_us)
{
    if (b.tx || b.sent) {
        uint64_t span = (b.end_us ? b.end_us : now_us) - b.start_us;
        printf("{\"bench\":\"tx\",\"sent\":%lu,\"len\":%lu,\"span_us\":%llu,\"frames_per_s\":%.1f,\"kB_per_s\":%.1f}\n",
               (unsigned long)b.sent, (unsigned long)b.len, (unsigned long long)span,
               span ? (double)b.sent * 1e6 / (double)span : 0.0,
               span ? (double)b.sent * (double)b.len * 1e6 / (double)span / 1000.0 : 0.0);
    }
    if (b.rx) {
        uint64_t span = b.seen ? b.last_rx_us - b.first_rx_us : 0;
        printf("{\"bench\":\"rx\",\"received\":%lu,\"lost\":%lu,\"bad\":%lu,\"bytes\":%lu,\"span_us\":%llu,"
               "\"frames_per_s\":%.1f,\"kB_per_s\":%.1f,\"gap_min_us\":%llu,\"gap_avg_us\":%llu,"
               "\"gap_max_us\":%llu,\"rssi_avg\":%.1f}\n",
               (unsigned long)b.received, (unsigned long)b.gaps, (unsigned long)b.bad, (unsigned long)b.bytes,
               (unsigned long long)span, span ? (double)(b.received - 1) * 1e6 / (double)span : 0.0,
               span ? (double)b.bytes * 1e6 / (double)span / 1000.0 : 0.0,
               (unsigned long long)(b.gap_min == UINT64_MAX ? 0 : b.gap_min),
               (unsigned long long)(b.received > 1 ? b.gap_sum / (b.received - 1) : 0),
               (unsigned long long)b.gap_max, b.received ? (double)b.rssi_sum / (double)b.received : 0.0);
    }
}
