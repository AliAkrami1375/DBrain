/*
 * DBrain node firmware for the ESP32 family.
 *
 * The node agent from core/ runs in its own task and is fed by the Wi-Fi
 * port; the main task reads console lines. The two meet under one mutex.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#else
#include "driver/uart_vfs.h"
#endif
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
static uint8_t applied_rate = 0xFF;

static void lock(void)   { xSemaphoreTake(node_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(node_lock); }

static int bench_send(const uint8_t *mac, const uint8_t *frame, size_t len)
{
    return port_wifi_send(mac, frame, len, FRAME_BENCH);
}

static void do_reboot(void)
{
    puts("rebooting");
    vTaskDelay(pdMS_TO_TICKS(100));
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
        if (node.cluster_valid && node.phy_rate != applied_rate) {
            applied_rate = node.phy_rate;
            port_wifi_set_rate(applied_rate);
        }
        if (gateway_mode() != 0) {
            uint32_t now = (uint32_t)(port_now_us() / 1000u);
            if (now - last_status >= 1000u) { last_status = now; gateway_status(&node); }
        }
        unlock();
    }
}

static void console_init(void)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&cfg);
    usb_serial_jtag_vfs_use_driver();
#else
    uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 512, 0, 0, NULL, 0);
    uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
    uart_vfs_dev_port_set_rx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM, ESP_LINE_ENDINGS_CRLF);
#endif
    setvbuf(stdin, NULL, _IONBF, 0);
}

void app_main(void)
{
    uint8_t channel;
    bool stream;
    cli_hooks_t hooks;
    node_platform_t pf;
    esp_err_t err;
    size_t arena_size;
    void *arena, *image;

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    storage_load_config(&config, &channel, &stream);

    arena_size = shard_arena_size(CONFIG_DBRAIN_MAX_NEURONS);
    arena = heap_caps_malloc(arena_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    image = heap_caps_malloc((size_t)CONFIG_DBRAIN_IMAGE_KB * 1024u, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (image == NULL)
        image = heap_caps_malloc((size_t)CONFIG_DBRAIN_IMAGE_KB * 1024u, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (arena == NULL || image == NULL) {
        ESP_LOGE(TAG, "not enough memory for %u neurons and a %u KB image", CONFIG_DBRAIN_MAX_NEURONS,
                 CONFIG_DBRAIN_IMAGE_KB);
        return;
    }
    config.arena = arena;
    config.arena_size = arena_size;
    config.image_buf = (uint8_t *)image;
    config.image_cap = (uint32_t)CONFIG_DBRAIN_IMAGE_KB * 1024u;
    ESP_LOGI(TAG, "arena %u bytes internal, image %u KB %s", (unsigned)arena_size, CONFIG_DBRAIN_IMAGE_KB,
             esp_ptr_external_ram(image) ? "in PSRAM" : "internal");

    if (config.mode == NODE_MODE_ARTIFACT) {
        if (!storage_map_artifact(&config.artifact, &config.artifact_size)) {
            ESP_LOGW(TAG, "artifact mode but no image in the artifact partition; running the recipe instead");
            config.mode = NODE_MODE_RECIPE;
        }
    }

    ESP_ERROR_CHECK(port_wifi_init(channel) == 0 ? ESP_OK : ESP_FAIL);
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

    ESP_LOGI(TAG, "DBrain %s, cluster %u, channel %u, %s mode", DBRAIN_VERSION, (unsigned)config.cluster_id,
             (unsigned)channel, config.mode == NODE_MODE_RECIPE ? "recipe" : "artifact");
    xTaskCreatePinnedToCore(node_task, "dbrain", 8192, NULL, 10, NULL, SOC_CPU_CORES_NUM > 1 ? 1 : 0);

    console_init();
    for (;;) {
        int c = fgetc(stdin);
        if (c == EOF) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        cli_feed_char((char)c);
    }
}
