/* Board side of the ESP32-family firmware: radio, clock, capability, storage. */
#ifndef DBRAIN_FW_PORT_H
#define DBRAIN_FW_PORT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dbrain/node.h"

/* A frame handed up from the Wi-Fi task. */
typedef struct {
    uint8_t  mac[6];
    uint8_t  len;
    bool     hw_ts;
    int8_t   rssi;
    uint64_t rx_us;
    uint8_t  data[250];
} port_rx_t;

typedef struct {
    uint8_t  tag;
    uint64_t tx_us;
    bool     ok;
} port_tx_done_t;

/* Brings up Wi-Fi in station mode on the channel and starts ESP-NOW. */
int  port_wifi_init(uint8_t channel);

/* Sends a frame; mac == NULL broadcasts. Peers are added on demand. */
int  port_wifi_send(const uint8_t *mac, const uint8_t *frame, size_t len, uint8_t tag);

/* Applies a PHY rate (PHY_RATE_*) to the broadcast peer and future peers. */
void port_wifi_set_rate(uint8_t phy_rate);

/* Blocking receive with timeout; returns true when a frame was dequeued. */
bool port_wifi_recv(port_rx_t *out, uint32_t timeout_ms);
bool port_wifi_tx_done(port_tx_done_t *out);

uint32_t port_wifi_rx_dropped(void);
void     port_wifi_mac(uint8_t mac[6]);

/* node_platform_t for this board; ctx is unused. */
void port_platform(node_platform_t *pf);

/* Chip and memory facts. */
void port_capability(cap_t *cap);
uint64_t port_now_us(void);

#endif
