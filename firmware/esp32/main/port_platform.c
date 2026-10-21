#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "soc/soc_caps.h"
#include "sdkconfig.h"
#include "port.h"
#include "gateway.h"

static const char *TAG = "node";

void port_capability(cap_t *cap)
{
    memset(cap, 0, sizeof(*cap));
#if CONFIG_IDF_TARGET_ESP32
    cap->chip = DBRAIN_CHIP_ESP32;
#elif CONFIG_IDF_TARGET_ESP32S2
    cap->chip = DBRAIN_CHIP_ESP32S2;
#elif CONFIG_IDF_TARGET_ESP32S3
    cap->chip = DBRAIN_CHIP_ESP32S3;
#elif CONFIG_IDF_TARGET_ESP32C2
    cap->chip = DBRAIN_CHIP_ESP32C2;
#elif CONFIG_IDF_TARGET_ESP32C3
    cap->chip = DBRAIN_CHIP_ESP32C3;
#elif CONFIG_IDF_TARGET_ESP32C5
    cap->chip = DBRAIN_CHIP_ESP32C5;
#elif CONFIG_IDF_TARGET_ESP32C6
    cap->chip = DBRAIN_CHIP_ESP32C6;
#else
    cap->chip = DBRAIN_CHIP_UNKNOWN;
#endif
    cap->cores = SOC_CPU_CORES_NUM;
    cap->mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    cap->free_heap = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#if CONFIG_SPIRAM
    cap->psram = (uint32_t)heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
#endif
    cap->flags = CAP_FLAG_HW_RX_TS;
#if SOC_CPU_HAS_FPU
    cap->flags |= CAP_FLAG_FPU;
#endif
    if (cap->psram) cap->flags |= CAP_FLAG_PSRAM;
    cap_classify(cap);
}

static uint64_t pf_now_us(void *ctx)
{
    (void)ctx;
    return port_now_us();
}

static int pf_send(void *ctx, const uint8_t *mac, const uint8_t *frame, size_t len, uint8_t tag)
{
    (void)ctx;
    return port_wifi_send(mac, frame, len, tag);
}

static void pf_get_mac(void *ctx, uint8_t mac[6])
{
    (void)ctx;
    port_wifi_mac(mac);
}

static void pf_get_cap(void *ctx, cap_t *cap)
{
    (void)ctx;
    port_capability(cap);
}

static uint32_t pf_random(void *ctx)
{
    (void)ctx;
    return esp_random();
}

static void pf_log(void *ctx, int level, const char *msg)
{
    (void)ctx;
    switch (level) {
    case NODE_LOG_ERROR: ESP_LOGE(TAG, "%s", msg); break;
    case NODE_LOG_WARN:  ESP_LOGW(TAG, "%s", msg); break;
    case NODE_LOG_INFO:  ESP_LOGI(TAG, "%s", msg); break;
    default:             ESP_LOGD(TAG, "%s", msg); break;
    }
}

static void pf_event(void *ctx, const node_event_t *ev)
{
    (void)ctx;
    gateway_event(ev);
}

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
