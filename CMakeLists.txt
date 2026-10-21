# Host build: the portable core as a library, its unit tests and the cluster
# simulator. Firmware builds live in firmware/esp32 and firmware/esp8266 and
# pull core/ in as a component; they do not use this file.
cmake_minimum_required(VERSION 3.16)
project(dbrain C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

option(DBRAIN_SANITIZE "Compile tests and tools with AddressSanitizer and UBSan" ON)
option(DBRAIN_WERROR "Treat warnings as errors" ON)

add_compile_options(-Wall -Wextra -Wshadow -Wstrict-prototypes -Wpointer-arith -Wcast-align
                    -Wundef -Wvla -Wformat=2)
if(DBRAIN_WERROR)
  add_compile_options(-Werror)
endif()
if(DBRAIN_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_subdirectory(core)
add_subdirectory(sim)

enable_testing()
add_subdirectory(tests)
