/*
 * Gateway stream: node events as JSON lines on the console.
 *
 * Any node can be the gateway; the host tool (host/dbrain/gateway.py) reads
 * these lines over the serial port and feeds the dashboard. Modes:
 *   0  off
 *   1  summary: one line per tick with spike counts, plus membership/sync events
 *   2  full: spike indices too (bounded per line)
 */
#ifndef DBRAIN_FW_GATEWAY_H
#define DBRAIN_FW_GATEWAY_H

#include <stdint.h>
#include "dbrain/node.h"

void gateway_init(uint8_t mode);
void gateway_set_mode(uint8_t mode);
uint8_t gateway_mode(void);
void gateway_event(const node_event_t *ev);

/* Periodic status line (membership, telemetry). Call about once per second. */
void gateway_status(const node_t *n);

#endif
