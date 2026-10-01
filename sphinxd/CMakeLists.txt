# MurmurHash3 是保持原样的第三方实现，其中的 switch 贯穿是有意设计。
set_source_files_properties(third_party/murmurhash3/MurmurHash3.cpp PROPERTIES COMPILE_FLAGS "-Wno-implicit-fallthrough")

if (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    set(SYSTEM_SOURCES src/reactor/reactor-epoll.cpp)
else ()
    message(FATAL_ERROR "Unsupported system: ${CMAKE_SYSTEM_NAME}")
endif (${CMAKE_SYSTEM_NAME} MATCHES "Linux")

include_directories(
        include
        SYSTEM third_party/murmurhash3
)

# 客户端与服务端仅共享哈希实现；商品服务不链接节点存储或 Reactor。
add_library(sphinx_hash STATIC third_party/murmurhash3/MurmurHash3.cpp)
target_include_directories(sphinx_hash SYSTEM PUBLIC third_party/murmurhash3)

add_library(sphinx_client STATIC
        src/client/cluster.cpp
        src/client/cluster_client.cpp
        src/client/cluster_resolver.cpp
        src/client/cluster_transport.cpp
)
target_include_directories(sphinx_client PUBLIC include)
target_link_libraries(sphinx_client PUBLIC sphinx_hash pthread)

add_library(sphinx_core STATIC
        src/common/buffer.cpp
        src/common/stats.cpp
        src/storage/logmem.cpp
        src/storage/memory.cpp
        src/reactor/reactor.cpp
        ${SYSTEM_SOURCES}
)
target_include_directories(sphinx_core PUBLIC include)
target_include_directories(sphinx_core SYSTEM PUBLIC third_party/murmurhash3)
target_link_libraries(sphinx_core PUBLIC sphinx_hash pthread)

target_precompile_headers(sphinx_core PUBLIC
        <algorithm>
        <atomic>
        <chrono>
        <cstdint>
        <memory>
        <string>
        <string_view>
        <unordered_map>
        <vector>
)

add_executable(sphinxd
        src/server/command_executor.cpp
        src/server/connection.cpp
        src/server/options.cpp
        src/server/server.cpp
        src/sphinxd.cpp
)
target_compile_definitions(sphinxd PRIVATE SPHINX_VERSION="v${SPHINX_VERSION}-${GIT_COMMIT_HASH}")
target_link_libraries(sphinxd PRIVATE sphinx_core pthread)

add_executable(sphinx-cluster
        src/sphinx-cluster.cpp
)
target_link_libraries(sphinx-cluster PRIVATE sphinx_client)

add_executable(sphinx_cluster_client_integration_driver
        test/client/cluster_client_integration_driver.cpp
)
target_link_libraries(sphinx_cluster_client_integration_driver PRIVATE sphinx_client)

if (GTEST_FOUND)
    add_executable(sphinxd_tests
            src/server/connection.cpp
            test/common/buffer_test.cpp
            test/storage/index_test.cpp
            test/client/cluster_test.cpp
            test/client/cluster_client_test.cpp
            test/server/connection_test.cpp
            test/storage/logmem_test.cpp
            test/main.cpp
            test/server/protocol_test.cpp
            test/reactor/reactor_test.cpp
            test/reactor/spsc_queue_test.cpp
            test/common/stats_test.cpp
            test/common/string_test.cpp
    )
    target_link_libraries(sphinxd_tests PRIVATE sphinx_core sphinx_client ${GTEST_BOTH_LIBRARIES} pthread)
    target_include_directories(sphinxd_tests PRIVATE src/server)
    add_test(sphinxd_tests sphinxd_tests)
endif ()

find_package(Python3 COMPONENTS Interpreter QUIET)
if (Python3_Interpreter_FOUND)
    add_test(NAME sphinxd_network_test
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/server/network_test.py
            $<TARGET_FILE:sphinxd>)
    set_tests_properties(sphinxd_network_test PROPERTIES TIMEOUT 180)

    add_test(NAME sphinxd_cluster_integration_test
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/client/cluster_network_test.py
            $<TARGET_FILE:sphinxd> $<TARGET_FILE:sphinx-cluster>
            $<TARGET_FILE:sphinx_cluster_client_integration_driver>)
    set_tests_properties(sphinxd_cluster_integration_test PROPERTIES TIMEOUT 120)
endif ()
