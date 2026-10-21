# DBrain firmware for the ESP32 family (ESP-IDF v5.5).
#
#   . $IDF_PATH/export.sh
#   idf.py set-target esp32s3      # esp32, esp32s2, esp32s3, esp32c2, esp32c3, esp32c5, esp32c6
#   idf.py build flash monitor
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../../core ${CMAKE_CURRENT_LIST_DIR}/../dbrain_common)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dbrain)
