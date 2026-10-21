/*
 * Transport benchmark (roadmap phase 0).
 *
 * `bench tx N LEN INTERVAL_US` on one board blasts N frames of LEN bytes;
 * `bench rx` on the others counts what arrives, with sequence gaps and
 * inter-arrival statistics. Bench frames use frame type 15, which the node
 * ignores, so a cluster can keep running underneath (though the numbers
 * then include its traffic).
 */
#ifndef DBRAIN_FW_BENCH_H
#define DBRAIN_FW_BENCH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define FRAME_BENCH 15

typedef int (*bench_send_fn)(const uint8_t *mac, const uint8_t *frame, size_t len);

void bench_init(bench_send_fn send);
int  bench_start_tx(uint32_t count, uint16_t len, uint32_t interval_us, uint64_t now_us);
void bench_start_rx(void);
void bench_stop(void);
bool bench_active(void);

/* Call often; sends the next frame when due. */
void bench_poll(uint64_t now_us);

/* Feed every received bench frame. */
void bench_on_frame(const uint8_t *data, size_t len, uint64_t rx_us, int8_t rssi);

/* Prints a JSON report. */
void bench_report(uint64_t now_us);

#endif
