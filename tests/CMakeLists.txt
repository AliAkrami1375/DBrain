set(DBRAIN_TESTS
    test_fix
    test_neuron
    test_bundle
    test_artifact
    test_shard
    test_membership
    test_timesync
    test_slot
    test_control
    test_recipe
    test_node)

foreach(t ${DBRAIN_TESTS})
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${t}.c)
    add_executable(${t} ${t}.c)
    if(${t} STREQUAL test_node)
      target_link_libraries(${t} dbrain_sim)
    else()
      target_link_libraries(${t} dbrain_core)
    endif()
    add_test(NAME ${t} COMMAND ${t})
  endif()
endforeach()
