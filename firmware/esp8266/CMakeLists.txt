# DBrain firmware for the ESP8266 (ESP8266_RTOS_SDK v3.4).
#
#   export IDF_PATH=/path/to/ESP8266_RTOS_SDK
#   export PATH=$PATH:/path/to/xtensa-lx106-elf/bin
#   python $IDF_PATH/tools/idf.py build flash monitor
cmake_minimum_required(VERSION 3.5)

set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../../core ${CMAKE_CURRENT_LIST_DIR}/../dbrain_common)
# CoAP is not needed and its submodules are not always available.
set(EXCLUDE_COMPONENTS coap)

# The ESP8266 has about 45 KB of heap for us: a 32-entry membership table and
# two in-flight control messages are plenty for a leaf.
add_compile_options(-DMEMBERSHIP_MAX=32 -DCTRL_INFLIGHT=2)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dbrain)
