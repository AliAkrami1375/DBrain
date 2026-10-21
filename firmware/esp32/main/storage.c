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

static uint32_t nvs_u32(nvs_handle_t h, const char *key, uint32_t dflt)
{
    uint32_t v = dflt;
    nvs_get_u32(h, key, &v);
    return v;
}

void storage_load_config(node_config_t *cfg, uint8_t *channel, bool *stream)
{
    nvs_handle_t h;

    node_config_default(cfg);
    cfg->cluster_id = CONFIG_DBRAIN_CLUSTER_ID;
#if CONFIG_DBRAIN_MODE_ARTIFACT
    cfg->mode = NODE_MODE_ARTIFACT;
#else
    cfg->mode = NODE_MODE_RECIPE;
#endif
#if CONFIG_DBRAIN_RATE_54M
    cfg->phy_rate = PHY_RATE_54M;
#elif CONFIG_DBRAIN_RATE_24M
    cfg->phy_rate = PHY_RATE_24M;
#elif CONFIG_DBRAIN_RATE_6M
    cfg->phy_rate = PHY_RATE_6M;
#else
    cfg->phy_rate = PHY_RATE_1M;
#endif
    cfg->slot_events = CONFIG_DBRAIN_SLOT_EVENTS;
    cfg->min_period_us = CONFIG_DBRAIN_MIN_PERIOD_US;
    cfg->tx_latency_us = CONFIG_DBRAIN_TX_LATENCY_US;
    cfg->discover_ms = CONFIG_DBRAIN_DISCOVER_MS;
    cfg->recipe.seed = CONFIG_DBRAIN_RECIPE_SEED;
    cfg->recipe.topology = CONFIG_DBRAIN_RECIPE_TOPOLOGY;
    cfg->recipe.local_fan_in = CONFIG_DBRAIN_RECIPE_LOCAL_FAN_IN;
    cfg->recipe.remote_fan_in = CONFIG_DBRAIN_RECIPE_REMOTE_FAN_IN;
    cfg->recipe.drive = CONFIG_DBRAIN_RECIPE_DRIVE;
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
    cfg->phy_rate = (uint8_t)nvs_u32(h, "rate", cfg->phy_rate);
    cfg->slot_events = (uint16_t)nvs_u32(h, "slot_ev", cfg->slot_events);
    cfg->min_period_us = nvs_u32(h, "min_per", cfg->min_period_us);
    cfg->tx_latency_us = (uint16_t)nvs_u32(h, "tx_lat", cfg->tx_latency_us);
    cfg->discover_ms = nvs_u32(h, "discover", cfg->discover_ms);
    cfg->recipe.seed = nvs_u32(h, "seed", cfg->recipe.seed);
    cfg->recipe.topology = (uint8_t)nvs_u32(h, "topology", cfg->recipe.topology);
    cfg->recipe.local_fan_in = (uint16_t)nvs_u32(h, "fan_local", cfg->recipe.local_fan_in);
    cfg->recipe.remote_fan_in = (uint16_t)nvs_u32(h, "fan_remote", cfg->recipe.remote_fan_in);
    cfg->recipe.drive = (uint16_t)nvs_u32(h, "drive", cfg->recipe.drive);
    *stream = nvs_u32(h, "stream", *stream ? 1u : 0u) != 0;
    nvs_close(h);
}

static const struct { const char *key; uint32_t min, max; } keys[] = {
    { "cluster", 1, 65535 }, { "channel", 1, 13 }, { "mode", 0, 1 }, { "rate", 0, 3 },
    { "slot_ev", 1, 119 }, { "min_per", 1000, 1000000 }, { "tx_lat", 0, 65535 },
    { "discover", 1000, 60000 }, { "seed", 0, 0xFFFFFFFFu }, { "topology", 0, 2 },
    { "fan_local", 0, 255 }, { "fan_remote", 0, 255 }, { "drive", 0, 65535 }, { "stream", 0, 1 },
};

int storage_set(const char *key, const char *value)
{
    nvs_handle_t h;
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
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
}

static const esp_partition_t *artifact_partition(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "artifact");
}

bool storage_map_artifact(const void **image, uint32_t *size)
{
    const esp_partition_t *part = artifact_partition();
    uint8_t hdr[ARTIFACT_HDR_LEN];
    uint32_t total;
    esp_partition_mmap_handle_t handle;
    const void *ptr;

    if (part == NULL) return false;
    if (esp_partition_read(part, 0, hdr, sizeof hdr) != ESP_OK) return false;
    if (memcmp(hdr, "DBRA", 4) != 0) return false;
    total = rd32(hdr + 24);
    if (total < ARTIFACT_HDR_LEN || total > part->size) return false;
    if (esp_partition_mmap(part, 0, total, ESP_PARTITION_MMAP_DATA, &ptr, &handle) != ESP_OK) return false;
    *image = ptr;
    *size = total;
    ESP_LOGI(TAG, "artifact partition mapped: %lu bytes", (unsigned long)total);
    return true;
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
