# The core is one directory that builds three ways:
#   - as an ESP-IDF component (idf_component_register),
#   - as an ESP8266 RTOS SDK component (register_component),
#   - as a plain static + shared library on the host.
set(DBRAIN_CORE_SRCS
    src/fix.c
    src/neuron.c
    src/crc32.c
    src/bundle.c
    src/artifact.c
    src/shard.c
    src/membership.c
    src/timesync.c
    src/slot.c
    src/control.c
    src/recipe.c
    src/node.c
    src/rng.c)

if(COMMAND idf_component_register)
  idf_component_register(SRCS ${DBRAIN_CORE_SRCS} INCLUDE_DIRS include)
elseif(COMMAND register_component)
  set(COMPONENT_SRCS ${DBRAIN_CORE_SRCS})
  set(COMPONENT_ADD_INCLUDEDIRS include)
  register_component()
else()
  add_library(dbrain_core STATIC ${DBRAIN_CORE_SRCS})
  target_include_directories(dbrain_core PUBLIC include)
  # The shared library is loaded by Python through ctypes, so it is built
  # without sanitizers even when the tests use them.
  add_library(dbrain_shared SHARED ${DBRAIN_CORE_SRCS})
  target_include_directories(dbrain_shared PUBLIC include)
  target_compile_options(dbrain_shared PRIVATE -fno-sanitize=all -O2)
  target_link_options(dbrain_shared PRIVATE -fno-sanitize=all)
  set_target_properties(dbrain_shared PROPERTIES OUTPUT_NAME dbrain)
endif()
