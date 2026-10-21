#include <string.h>
#include <stdlib.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "storage.h"
#include "dbrain/artifact.h"
#include "dbrain/frame.h"
#include "dbrain/slot.h"

static const char *TAG = "storage";
#define NVS_NS "dbrain"

static uint32_t nvs_u32(nvs_handle h, const char *key, uint32_t dflt)
{
    uint32_t v = dflt;
    nvs_get_u32(h, key, &v);
    return v;
}

void storage_load_config(node_config_t *cfg, uint8_t *channel, bool *stream)
{
    nvs_handle h;

    node_config_default(cfg);
    cfg->cluster_id = CONFIG_DBRAIN_CLUSTER_ID;
#if CONFIG_DBRAIN_MODE_ARTIFACT
    cfg->mode = NODE_MODE_ARTIFACT;
#else
    cfg->mode = NODE_MODE_RECIPE;
#endif
    cfg->phy_rate = PHY_RATE_1M;
    cfg->slot_events = CONFIG_DBRAIN_SLOT_EVENTS;
    cfg->min_period_us = CONFIG_DBRAIN_MIN_PERIOD_US;
    cfg->discover_ms = CONFIG_DBRAIN_DISCOVER_MS;
    *channel = CONFIG_DBRAIN_CHANNEL;
#if CONFIG_DBRAIN_STREAM_DEFAULT
    *stream = true;
#else
    *stream = false;
#endif
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    cfg->cluster_id = (uint16_t)nvs_u32(h, "cluster", cfg->cluster_id);
    *channel = (uint8_t)nvs_u32(h, "channel", *channel);
    cfg->mode = (uint8_t)nvs_u32(h, "mode", cfg->mode);
    cfg->slot_events = (uint16_t)nvs_u32(h, "slot_ev", cfg->slot_events);
    cfg->min_period_us = nvs_u32(h, "min_per", cfg->min_period_us);
    cfg->discover_ms = nvs_u32(h, "discover", cfg->discover_ms);
    *stream = nvs_u32(h, "stream", *stream ? 1u : 0u) != 0;
    nvs_close(h);
}

static const struct { const char *key; uint32_t min, max; } keys[] = {
    { "cluster", 1, 65535 }, { "channel", 1, 13 }, { "mode", 0, 1 }, { "slot_ev", 1, 119 },
    { "min_per", 1000, 1000000 }, { "discover", 1000, 60000 }, { "stream", 0, 1 },
};

int storage_set(const char *key, const char *value)
{
    nvs_handle h;
    char *end = NULL;
    unsigned long v = strtoul(value, &end, 0);
    unsigned i;

    if (end == value || *end != 0) return -1;
    for (i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        if (strcmp(keys[i].key, key) != 0) continue;
        if (v < keys[i].min || v > keys[i].max) return -1;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return -1;
        nvs_set_u32(h, key, (uint32_t)v);
        nvs_commit(h);
        nvs_close(h);
        return 0;
    }
    return -1;
}

void storage_erase(void)
{
    nvs_handle h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
}

static const esp_partition_t *artifact_partition(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "artifact");
}

uint32_t storage_read_artifact(void *buf, uint32_t cap)
{
    const esp_partition_t *part = artifact_partition();
    uint8_t hdr[ARTIFACT_HDR_LEN];
    uint32_t total;

    if (part == NULL) return 0;
    if (esp_partition_read(part, 0, hdr, sizeof hdr) != ESP_OK) return 0;
    if (memcmp(hdr, "DBRA", 4) != 0) return 0;
    total = rd32(hdr + 24);
    if (total < ARTIFACT_HDR_LEN || total > part->size || total > cap) {
        ESP_LOGW(TAG, "artifact of %lu bytes does not fit the %lu byte buffer", (unsigned long)total,
                 (unsigned long)cap);
        return 0;
    }
    if (esp_partition_read(part, 0, buf, total) != ESP_OK) return 0;
    return total;
}

int storage_write_artifact(const void *image, uint32_t size)
{
    const esp_partition_t *part = artifact_partition();
    uint32_t erase = (size + 0xFFFu) & ~0xFFFu;

    if (part == NULL || size > part->size) return -1;
    if (esp_partition_erase_range(part, 0, erase) != ESP_OK) return -1;
    if (esp_partition_write(part, 0, image, size) != ESP_OK) return -1;
    return 0;
}
