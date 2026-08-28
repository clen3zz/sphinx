option(BUILD_BENCHMARKS "Build Sphinx benchmarks" OFF)

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/include/sphinx)

set(PROTOCOL_HEADER ${CMAKE_CURRENT_BINARY_DIR}/include/sphinx/protocol.h)
set(PROTOCOL_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/src/protocol.rl)

add_custom_command(
  OUTPUT ${PROTOCOL_HEADER}
  COMMAND ${RAGEL_EXECUTABLE} -o ${PROTOCOL_HEADER} ${PROTOCOL_SOURCE}
  DEPENDS ${PROTOCOL_SOURCE}
  COMMENT "Generating protocol parser with Ragel"
  VERBATIM
)
add_custom_target(generate_protocol DEPENDS ${PROTOCOL_HEADER})

if(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
  set(SYSTEM_SOURCES src/reactor-epoll.cpp)
else()
    message(FATAL_ERROR "Unsupported system: ${CMAKE_SYSTEM_NAME}")
endif(${CMAKE_SYSTEM_NAME} MATCHES "Linux")

include_directories(
    include
    ${CMAKE_CURRENT_BINARY_DIR}/include
)

add_executable(sphinxd
    src/MurmurHash3.cpp
    src/buffer.cpp
    src/stats.cpp
    src/logmem.cpp
    src/memory.cpp
    src/reactor.cpp
    src/server/command_executor.cpp
    src/server/connection.cpp
    src/server/options.cpp
    src/server/server.cpp
    src/sphinxd.cpp
    ${SYSTEM_SOURCES}
)
add_dependencies(sphinxd generate_protocol)
target_compile_definitions(sphinxd PRIVATE SPHINX_VERSION="v${SPHINX_VERSION}-${GIT_COMMIT_HASH}")
target_link_libraries(sphinxd pthread)

add_executable(sphinx-cluster
    src/MurmurHash3.cpp
    src/cluster.cpp
    src/cluster_client.cpp
    src/sphinx-cluster.cpp
)
target_link_libraries(sphinx-cluster pthread)

add_executable(sphinx_cluster_client_integration_driver
    src/MurmurHash3.cpp
    src/cluster.cpp
    src/cluster_client.cpp
    test/cluster_client_integration_driver.cpp
)
target_link_libraries(sphinx_cluster_client_integration_driver pthread)

if(GTEST_FOUND)
  add_executable(sphinxd_tests
      src/MurmurHash3.cpp
      src/buffer.cpp
      src/stats.cpp
      src/cluster.cpp
      src/cluster_client.cpp
      src/logmem.cpp
      src/memory.cpp
      src/reactor.cpp
      src/reactor-epoll.cpp
      test/buffer_test.cpp
      test/index_test.cpp
      test/cluster_test.cpp
      test/cluster_client_test.cpp
      test/logmem_test.cpp
      test/main.cpp
      test/protocol_test.cpp
      test/reactor_test.cpp
      test/spsc_queue_test.cpp
      test/stats_test.cpp
      test/string_test.cpp
  )
  add_dependencies(sphinxd_tests generate_protocol)
  target_link_libraries(sphinxd_tests ${GTEST_BOTH_LIBRARIES} pthread)
  add_test(sphinxd_tests sphinxd_tests)
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME sphinxd_network_test
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/network_test.py
                   $<TARGET_FILE:sphinxd>)
  set_tests_properties(sphinxd_network_test PROPERTIES TIMEOUT 180)

  add_test(NAME sphinxd_benchmark_script_test
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/benchmark_script_test.py)
  set_tests_properties(sphinxd_benchmark_script_test PROPERTIES TIMEOUT 30)

  find_program(MEMTIER_BENCHMARK_EXECUTABLE memtier_benchmark)
  if(MEMTIER_BENCHMARK_EXECUTABLE)
    add_test(NAME sphinxd_benchmark_smoke
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/run_local_benchmark.py
                     --server $<TARGET_FILE:sphinxd>
                     --memtier ${MEMTIER_BENCHMARK_EXECUTABLE}
                     --workload mixed
                     --threads 1
                     --memory-limit 8
                     --segment-size 1
                     --duration 3
                     --client-threads 1
                     --connections 2
                     --key-space 100
                     --value-size 16
                     --output-dir ${CMAKE_CURRENT_BINARY_DIR}/benchmark-smoke)
    set_tests_properties(sphinxd_benchmark_smoke PROPERTIES TIMEOUT 60)
  endif()

  foreach(_sphinx_cluster_integration_name IN ITEMS
          cluster_network_test.py
          cluster_integration_test.py)
    set(_sphinx_cluster_integration_test
        ${CMAKE_CURRENT_SOURCE_DIR}/test/${_sphinx_cluster_integration_name})
    if(EXISTS ${_sphinx_cluster_integration_test})
      add_test(NAME sphinxd_cluster_integration_test
               COMMAND ${Python3_EXECUTABLE} ${_sphinx_cluster_integration_test}
                       $<TARGET_FILE:sphinxd> $<TARGET_FILE:sphinx-cluster>
                       $<TARGET_FILE:sphinx_cluster_client_integration_driver>)
      set_tests_properties(sphinxd_cluster_integration_test PROPERTIES TIMEOUT 120)
      break()
    endif()
  endforeach()
endif()

if(BUILD_BENCHMARKS)
  add_executable(sphinxd_perf
      perf/buffer_perf.cpp
      perf/logmem_perf.cpp
      perf/main.cpp
      perf/protocol_perf.cpp
      src/MurmurHash3.cpp
      src/buffer.cpp
      src/logmem.cpp
      src/memory.cpp
  )
  add_dependencies(sphinxd_perf generate_protocol)
  target_link_libraries(sphinxd_perf benchmark pthread)
endif()
