# Virtual radio and cluster simulator. libdbrain_sim is reused by the tests.
add_library(dbrain_sim STATIC vnet.c)
target_include_directories(dbrain_sim PUBLIC .)
target_link_libraries(dbrain_sim dbrain_core)

add_executable(dbrain-sim main.c)
target_link_libraries(dbrain-sim dbrain_sim)
