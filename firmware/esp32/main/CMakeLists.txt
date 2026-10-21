idf_component_register(
    SRCS main.c port_wifi.c port_platform.c storage.c
    INCLUDE_DIRS .
    REQUIRES core dbrain_common esp_wifi esp_event esp_netif nvs_flash esp_timer esp_partition
             spi_flash driver vfs esp_hw_support)
