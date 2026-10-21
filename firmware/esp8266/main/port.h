/* Board side of the ESP8266 firmware. Same contract as the ESP32 port. */
#ifndef DBRAIN_FW_PORT_H
#define DBRAIN_FW_PORT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dbrain/node.h"

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

int  port_wifi_init(uint8_t channel);
int  port_wifi_send(const uint8_t *mac, const uint8_t *frame, size_t len, uint8_t tag);
bool port_wifi_recv(port_rx_t *out, uint32_t timeout_ms);
bool port_wifi_tx_done(port_tx_done_t *out);
uint32_t port_wifi_rx_dropped(void);
void port_wifi_mac(uint8_t mac[6]);
void port_platform(node_platform_t *pf);
void port_capability(cap_t *cap);
uint64_t port_now_us(void);

#endif
