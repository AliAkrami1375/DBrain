/* DBrain node firmware for the ESP8266: a leaf-tier member of the cluster. */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "esp_vfs_dev.h"
#include "sdkconfig.h"
#include "port.h"
#include "storage.h"
#include "cli.h"
#include "gateway.h"
#include "bench.h"
#include "dbrain/frame.h"
#include "dbrain/version.h"

static const char *TAG = "dbrain";
static node_t node;
static node_config_t config;
static SemaphoreHandle_t node_lock;

static void lock(void)   { xSemaphoreTake(node_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(node_lock); }

static int bench_send(const uint8_t *mac, const uint8_t *frame, size_t len)
{
    return port_wifi_send(mac, frame, len, FRAME_BENCH);
}

static void do_reboot(void)
{
    puts("rebooting");
    vTaskDelay(100 / portTICK_PERIOD_MS);
    esp_restart();
}

static void node_task(void *arg)
{
    port_rx_t rx;
    port_tx_done_t done;
    uint32_t last_status = 0;

    (void)arg;
    lock();
    node_start(&node);
    unlock();
    for (;;) {
        bool got = port_wifi_recv(&rx, 1);
        lock();
        while (port_wifi_tx_done(&done)) node_on_sent(&node, done.tag, done.tx_us);
        while (got) {
            if (frame_type(rx.data, rx.len) == FRAME_BENCH)
                bench_on_frame(rx.data, rx.len, rx.rx_us, rx.rssi);
            else
                node_on_frame(&node, rx.mac, rx.data, rx.len, rx.rx_us, rx.hw_ts);
            got = port_wifi_recv(&rx, 0);
        }
        node_poll(&node);
        bench_poll(port_now_us());
        if (gateway_mode() != 0) {
            uint32_t now = (uint32_t)(port_now_us() / 1000u);
            if (now - last_status >= 1000u) { last_status = now; gateway_status(&node); }
        }
        unlock();
    }
}

void app_main(void)
{
    uint8_t channel;
    bool stream;
    cli_hooks_t hooks;
    node_platform_t pf;
    esp_err_t err;
    size_t arena_size;
    void *mem, *image;

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    storage_load_config(&config, &channel, &stream);
    arena_size = shard_arena_size(CONFIG_DBRAIN_MAX_NEURONS);
    mem = malloc(arena_size);
    image = malloc((size_t)CONFIG_DBRAIN_IMAGE_KB * 1024u);
    if (mem == NULL || image == NULL) {
        ESP_LOGE(TAG, "not enough memory for %u neurons and a %u KB image", CONFIG_DBRAIN_MAX_NEURONS,
                 CONFIG_DBRAIN_IMAGE_KB);
        return;
    }
    config.arena = mem;
    config.arena_size = arena_size;
    config.image_buf = (uint8_t *)image;
    config.image_cap = (uint32_t)CONFIG_DBRAIN_IMAGE_KB * 1024u;

    if (config.mode == NODE_MODE_ARTIFACT) {
        uint32_t size = storage_read_artifact(image, config.image_cap);
        if (size) {
            config.artifact = image;
            config.artifact_size = size;
        } else {
            ESP_LOGW(TAG, "artifact mode but no usable image; running the recipe instead");
            config.mode = NODE_MODE_RECIPE;
        }
    }

    if (port_wifi_init(channel) != 0) {
        ESP_LOGE(TAG, "radio init failed");
        return;
    }
    port_platform(&pf);
    if (node_init(&node, &pf, &config) != 0) {
        ESP_LOGE(TAG, "node init failed");
        return;
    }
    gateway_init(stream ? 1 : 0);
    bench_init(bench_send);

    node_lock = xSemaphoreCreateMutex();
    memset(&hooks, 0, sizeof hooks);
    hooks.node = &node;
    hooks.set = storage_set;
    hooks.erase = storage_erase;
    hooks.reboot = do_reboot;
    hooks.save_artifact = storage_write_artifact;
    hooks.send_raw = bench_send;
    hooks.now_us = port_now_us;
    hooks.rx_dropped = port_wifi_rx_dropped;
    hooks.lock = lock;
    hooks.unlock = unlock;
    cli_init(&hooks);

    ESP_LOGI(TAG, "DBrain %s, cluster %u, channel %u, %s mode, free heap %u", DBRAIN_VERSION,
             (unsigned)config.cluster_id, (unsigned)channel,
             config.mode == NODE_MODE_RECIPE ? "recipe" : "artifact", (unsigned)esp_get_free_heap_size());
    xTaskCreate(node_task, "dbrain", 4096, NULL, 10, NULL);

    uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
    esp_vfs_dev_uart_use_driver(UART_NUM_0);
    esp_vfs_dev_uart_set_rx_line_endings(ESP_LINE_ENDINGS_CRLF);
    setvbuf(stdin, NULL, _IONBF, 0);
    for (;;) {
        int c = fgetc(stdin);
        if (c == EOF) { vTaskDelay(10 / portTICK_PERIOD_MS); continue; }
        cli_feed_char((char)c);
    }
}
