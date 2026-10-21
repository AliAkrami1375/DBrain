#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"
#include "tcpip_adapter.h"
#include "sdkconfig.h"
#include "port.h"
#include "gateway.h"

static const char *TAG = "port";

#define RX_QUEUE_LEN 12
#define TX_QUEUE_LEN 8
#define PEER_CACHE   6

static QueueHandle_t rx_queue;
static QueueHandle_t tx_queue;
static uint32_t rx_dropped;
static const uint8_t broadcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static uint8_t tag_fifo[TX_QUEUE_LEN];
static volatile unsigned tag_head, tag_tail;
static struct { uint8_t mac[6]; uint32_t used; bool valid; } peers[PEER_CACHE];
static uint32_t peer_clock;

uint64_t port_now_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

/* No hardware receive timestamp on this chip: the callback entry time is the sample. */
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

static void send_cb(const uint8_t *mac, esp_now_send_status_t status)
{
    port_tx_done_t item;

    (void)mac;
    item.tx_us = port_now_us();
    item.ok = status == ESP_NOW_SEND_SUCCESS;
    item.tag = 0;
    if (tag_tail != tag_head) {
        item.tag = tag_fifo[tag_tail % TX_QUEUE_LEN];
        tag_tail++;
    }
    xQueueSend(tx_queue, &item, 0);
}

static int add_peer(const uint8_t *mac)
{
    esp_now_peer_info_t p;

    memset(&p, 0, sizeof p);
    memcpy(p.peer_addr, mac, 6);
    p.channel = 0;
    p.ifidx = ESP_IF_WIFI_STA;
    p.encrypt = false;
    return esp_now_add_peer(&p) == ESP_OK ? 0 : -1;
}

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

    tcpip_adapter_init();
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

int port_wifi_send(const uint8_t *mac, const uint8_t *frame, size_t len, uint8_t tag)
{
    if (len == 0 || len > 250) return -1;
    if (mac != NULL && ensure_peer(mac) != 0) return -1;
    if ((unsigned)(tag_head - tag_tail) < TX_QUEUE_LEN) tag_fifo[tag_head % TX_QUEUE_LEN] = tag;
    tag_head++;
    /* the SDK wants a writable pointer; it does not modify the buffer */
    if (esp_now_send((uint8_t *)(mac ? mac : broadcast_mac), (uint8_t *)frame, len) != ESP_OK) {
        tag_head--;
        return -1;
    }
    return 0;
}

bool port_wifi_recv(port_rx_t *out, uint32_t timeout_ms)
{
    return xQueueReceive(rx_queue, out, timeout_ms / portTICK_PERIOD_MS) == pdTRUE;
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
    esp_wifi_get_mac(ESP_IF_WIFI_STA, mac);
}

void port_capability(cap_t *cap)
{
    memset(cap, 0, sizeof(*cap));
    cap->chip = DBRAIN_CHIP_ESP8266;
    cap->cores = 1;
#ifdef CONFIG_ESP8266_DEFAULT_CPU_FREQ_160
    cap->mhz = 160;
#else
    cap->mhz = 80;
#endif
    cap->free_heap = esp_get_free_heap_size();
    cap->psram = 0;
    cap->flags = 0;
    cap_classify(cap);
}

static uint64_t pf_now_us(void *ctx) { (void)ctx; return port_now_us(); }
static int pf_send(void *ctx, const uint8_t *mac, const uint8_t *frame, size_t len, uint8_t tag)
{ (void)ctx; return port_wifi_send(mac, frame, len, tag); }
static void pf_get_mac(void *ctx, uint8_t mac[6]) { (void)ctx; port_wifi_mac(mac); }
static void pf_get_cap(void *ctx, cap_t *cap) { (void)ctx; port_capability(cap); }
static uint32_t pf_random(void *ctx) { (void)ctx; return esp_random(); }
static void pf_log(void *ctx, int level, const char *msg)
{
    (void)ctx;
    switch (level) {
    case NODE_LOG_ERROR: ESP_LOGE("node", "%s", msg); break;
    case NODE_LOG_WARN:  ESP_LOGW("node", "%s", msg); break;
    case NODE_LOG_INFO:  ESP_LOGI("node", "%s", msg); break;
    default:             ESP_LOGD("node", "%s", msg); break;
    }
}
static void pf_event(void *ctx, const node_event_t *ev) { (void)ctx; gateway_event(ev); }

void port_platform(node_platform_t *pf)
{
    memset(pf, 0, sizeof(*pf));
    pf->now_us = pf_now_us;
    pf->send = pf_send;
    pf->get_mac = pf_get_mac;
    pf->get_cap = pf_get_cap;
    pf->random = pf_random;
    pf->log = pf_log;
    pf->event = pf_event;
}
