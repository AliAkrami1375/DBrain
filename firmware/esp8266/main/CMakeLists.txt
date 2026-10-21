# pthread is listed so that libpthread.a lands inside the link group:
# the toolchain's libstdc++ guard code needs its condition variables.
idf_component_register(
    SRCS main.c port.c storage.c
    INCLUDE_DIRS .
    REQUIRES core dbrain_common esp8266 nvs_flash spi_flash vfs pthread)
