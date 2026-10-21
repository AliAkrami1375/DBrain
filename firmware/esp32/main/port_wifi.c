#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "port.h"
#include "dbrain/slot.h"

static const char *TAG = "port";

#define RX_QUEUE_LEN 32
#define TX_QUEUE_LEN 16
#define PEER_CACHE   16

static QueueHandle_t rx_queue;
static QueueHandle_t tx_queue;
static uint32_t rx_dropped;
static const uint8_t broadcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static uint8_t current_rate = PHY_RATE_1M;

/* tags of frames handed to the driver, consumed in order by the send callback */
static uint8_t tag_fifo[TX_QUEUE_LEN];
static volatile unsigned tag_head, tag_tail;

static struct { uint8_t mac[6]; uint32_t used; bool valid; } peers[PEER_CACHE];
static uint32_t peer_clock;

uint64_t port_now_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

/* The radio stamps arrival with a 32-bit microsecond counter; widen it against our clock. */
static uint64_t widen_timestamp(uint32_t ts32, uint64_t now)
{
    uint32_t delta = (uint32_t)now - ts32;
    return now - delta;
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    port_rx_t item;
    uint64_t now = port_now_us();

    if (len <= 0 || len > 250) return;
    memcpy(item.mac, info->src_addr, 6);
    item.len = (uint8_t)len;
    memcpy(item.data, data, (size_t)len);
    item.rssi = info->rx_ctrl ? (int8_t)info->rx_ctrl->rssi : 0;
    if (info->rx_ctrl && info->rx_ctrl->timestamp != 0) {
        item.hw_ts = true;
        item.rx_us = widen_timestamp(info->rx_ctrl->timestamp, now);
    } else {
        item.hw_ts = false;
        item.rx_us = now;
    }
    if (xQueueSend(rx_queue, &item, 0) != pdTRUE) rx_dropped++;
}
#else
static void recv_cb(const uint8_t *mac, const uint8_t *data, int len)
{
    port_rx_t item;

    if (len <= 0 || len > 250) return;
    memcpy(item.mac, mac, 6);
    item.len = (uint8_t)len;
    memcpy(item.data, data, (size_t)len);
    item.rssi = 0;
    item.hw_ts = false;
    item.rx_us = port_now_us();
    if (xQueueSend(rx_queue, &item, 0) != pdTRUE) rx_dropped++;
}
#endif

static void tx_done(esp_now_send_status_t status)
{
    port_tx_done_t item;

    item.tx_us = port_now_us();
    item.ok = status == ESP_NOW_SEND_SUCCESS;
    item.tag = 0;
    if (tag_tail != tag_head) {
        item.tag = tag_fifo[tag_tail % TX_QUEUE_LEN];
        tag_tail++;
    }
    xQueueSend(tx_queue, &item, 0);
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
static void send_cb(const wifi_tx_info_t *info, esp_now_send_status_t status)
{
    (void)info;
    tx_done(status);
}
#else
static void send_cb(const uint8_t *mac, esp_now_send_status_t status)
{
    (void)mac;
    tx_done(status);
}
#endif

static void apply_rate(const uint8_t *mac)
{
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
    esp_now_rate_config_t rc;
    memset(&rc, 0, sizeof rc);
    switch (current_rate) {
    case PHY_RATE_6M:  rc.phymode = WIFI_PHY_MODE_11G; rc.rate = WIFI_PHY_RATE_6M; break;
    case PHY_RATE_24M: rc.phymode = WIFI_PHY_MODE_11G; rc.rate = WIFI_PHY_RATE_24M; break;
    case PHY_RATE_54M: rc.phymode = WIFI_PHY_MODE_11G; rc.rate = WIFI_PHY_RATE_54M; break;
    default:           rc.phymode = WIFI_PHY_MODE_11B; rc.rate = WIFI_PHY_RATE_1M_L; break;
    }
    rc.ersu = false;
    rc.dcm = false;
    if (esp_now_set_peer_rate_config(mac, &rc) != ESP_OK)
        ESP_LOGW(TAG, "rate config rejected for %02x:%02x", mac[4], mac[5]);
#else
    (void)mac;
#endif
}

static int add_peer(const uint8_t *mac)
{
    esp_now_peer_info_t p;

    memset(&p, 0, sizeof p);
    memcpy(p.peer_addr, mac, 6);
    p.channel = 0;
    p.ifidx = WIFI_IF_STA;
    p.encrypt = false;
    if (esp_now_add_peer(&p) != ESP_OK) return -1;
    apply_rate(mac);
    return 0;
}

/* Unicast needs a peer entry; keep the most recently used ones. */
static int ensure_peer(const uint8_t *mac)
{
    unsigned i, victim = 0;
    uint32_t oldest = 0xFFFFFFFFu;

    peer_clock++;
    for (i = 0; i < PEER_CACHE; i++) {
        if (peers[i].valid && memcmp(peers[i].mac, mac, 6) == 0) {
            peers[i].used = peer_clock;
            return 0;
        }
    }
    for (i = 0; i < PEER_CACHE; i++) {
        if (!peers[i].valid) { victim = i; break; }
        if (peers[i].used < oldest) { oldest = peers[i].used; victim = i; }
    }
    if (peers[victim].valid) esp_now_del_peer(peers[victim].mac);
    if (add_peer(mac) != 0) return -1;
    memcpy(peers[victim].mac, mac, 6);
    peers[victim].used = peer_clock;
    peers[victim].valid = true;
    return 0;
}

int port_wifi_init(uint8_t channel)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    rx_queue = xQueueCreate(RX_QUEUE_LEN, sizeof(port_rx_t));
    tx_queue = xQueueCreate(TX_QUEUE_LEN, sizeof(port_tx_done_t));
    if (!rx_queue || !tx_queue) return -1;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));
    ESP_ERROR_CHECK(esp_now_register_send_cb(send_cb));
    if (add_peer(broadcast_mac) != 0) return -1;
    ESP_LOGI(TAG, "ESP-NOW up on channel %u", (unsigned)channel);
    return 0;
}

void port_wifi_set_rate(uint8_t phy_rate)
{
    unsigned i;

    if (phy_rate == current_rate) return;
    current_rate = phy_rate;
    apply_rate(broadcast_mac);
    for (i = 0; i < PEER_CACHE; i++)
        if (peers[i].valid) apply_rate(peers[i].mac);
    ESP_LOGI(TAG, "PHY rate index %u", (unsigned)phy_rate);
}

int port_wifi_send(const uint8_t *mac, const uint8_t *frame, size_t len, uint8_t tag)
{
    esp_err_t err;

    if (len == 0 || len > 250) return -1;
    if (mac != NULL && ensure_peer(mac) != 0) return -1;
    if ((unsigned)(tag_head - tag_tail) < TX_QUEUE_LEN) tag_fifo[tag_head % TX_QUEUE_LEN] = tag;
    tag_head++;
    err = esp_now_send(mac ? mac : broadcast_mac, frame, len);
    if (err != ESP_OK) {
        tag_head--;
        ESP_LOGW(TAG, "send failed: %s", esp_err_to_name(err));
        return -1;
    }
    return 0;
}

bool port_wifi_recv(port_rx_t *out, uint32_t timeout_ms)
{
    return xQueueReceive(rx_queue, out, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

bool port_wifi_tx_done(port_tx_done_t *out)
{
    return xQueueReceive(tx_queue, out, 0) == pdTRUE;
}

uint32_t port_wifi_rx_dropped(void)
{
    return rx_dropped;
}

void port_wifi_mac(uint8_t mac[6])
{
    esp_wifi_get_mac(WIFI_IF_STA, mac);
}
