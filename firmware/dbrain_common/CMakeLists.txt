# Board-independent firmware pieces: console commands, gateway stream, transport benchmark.
set(DBRAIN_FW_SRCS cli.c gateway.c bench.c)

if(COMMAND idf_component_register)
  idf_component_register(SRCS ${DBRAIN_FW_SRCS} INCLUDE_DIRS . REQUIRES core)
elseif(COMMAND register_component)
  set(COMPONENT_SRCS ${DBRAIN_FW_SRCS})
  set(COMPONENT_ADD_INCLUDEDIRS .)
  set(COMPONENT_REQUIRES core)
  register_component()
endif()
