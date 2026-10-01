include(FetchContent)
FetchContent_Populate(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.12.0
        GIT_SHALLOW TRUE)
add_library(sphinx_json INTERFACE)
target_include_directories(sphinx_json INTERFACE ${nlohmann_json_SOURCE_DIR}/include)

find_package(PkgConfig REQUIRED)
pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis>=1.0.0)
pkg_check_modules(MYSQLCLIENT REQUIRED IMPORTED_TARGET mysqlclient)

# 这两个依赖仅使用头文件：拉取源码即可，不引入它们自己的 CMake 项目。
# 完整参数形式也兼容根项目要求的最低 CMake 3.16 版本。
FetchContent_Populate(cpp_httplib
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
        GIT_TAG v0.56.0
        GIT_SHALLOW TRUE)
add_library(sphinx_httplib INTERFACE)
target_include_directories(sphinx_httplib INTERFACE ${cpp_httplib_SOURCE_DIR})

# 业务核心只依赖统一编码与线程支持，不包含任何后端客户端。
add_library(sphinx_product_core STATIC
        src/domain/product_rules.cpp
        src/application/ports/product_cache.cpp
        src/application/ports/product_store.cpp
        src/application/cache/product_codec.cpp
        src/application/cache/product_cache_policy.cpp
        src/application/product_service.cpp
        src/application/product_metrics.cpp
        src/application/protection/product_read_coordinator.cpp
        src/application/protection/cache_circuit_breaker.cpp
)
target_include_directories(sphinx_product_core PUBLIC include)
target_link_libraries(sphinx_product_core PUBLIC sphinx_json pthread)

add_library(sphinx_product_mysql STATIC
        src/backends/mysql/mysql_product_store.cpp
        src/backends/mysql/mysql_runtime.cpp
)
target_link_libraries(sphinx_product_mysql PUBLIC sphinx_product_core PRIVATE PkgConfig::MYSQLCLIENT)

add_library(sphinx_product_redis STATIC src/backends/redis/redis_product_cache.cpp)
target_link_libraries(sphinx_product_redis PUBLIC sphinx_product_core PRIVATE PkgConfig::HIREDIS)

add_library(sphinx_product_sphinx STATIC src/backends/sphinx/sphinx_product_cache.cpp)
target_link_libraries(sphinx_product_sphinx PUBLIC sphinx_product_core sphinx_client)

add_library(sphinx_product_http STATIC
        src/http/product_http.cpp
        src/http/product_http_routes.cpp
)
target_link_libraries(sphinx_product_http PUBLIC sphinx_product_core PRIVATE sphinx_httplib sphinx_json)

add_library(sphinx_product_bootstrap STATIC
        src/bootstrap/product_cache_factory.cpp
        src/bootstrap/product_config.cpp
        src/bootstrap/product_worker.cpp
)
target_link_libraries(sphinx_product_bootstrap PUBLIC sphinx_product_core sphinx_product_http
        PRIVATE sphinx_product_mysql sphinx_product_redis sphinx_product_sphinx)

add_executable(sphinx-product-service src/bootstrap/product_main.cpp)
target_link_libraries(sphinx-product-service PRIVATE sphinx_product_bootstrap)

if (GTEST_FOUND)
    add_executable(sphinx_product_tests
            test/application/ports/product_cache_test.cpp
            test/application/ports/product_store_test.cpp
            test/application/cache/product_codec_test.cpp
            test/application/cache/product_cache_policy_test.cpp
            test/application/product_service_test.cpp
            test/application/product_metrics_test.cpp
            test/application/protection/cache_circuit_breaker_test.cpp
            test/application/protection/product_read_coordinator_test.cpp
    )
    target_link_libraries(sphinx_product_tests PRIVATE sphinx_product_core ${GTEST_BOTH_LIBRARIES})
    add_test(NAME sphinx_product_tests COMMAND sphinx_product_tests)

    add_executable(sphinx_product_backend_tests
            test/bootstrap/product_cache_factory_test.cpp
            test/backends/redis/redis_product_cache_test.cpp
            test/backends/sphinx/sphinx_product_cache_test.cpp
    )
    target_link_libraries(sphinx_product_backend_tests PRIVATE sphinx_product_bootstrap
            sphinx_product_redis sphinx_product_sphinx PkgConfig::HIREDIS ${GTEST_BOTH_LIBRARIES})
    add_test(NAME sphinx_product_backend_tests COMMAND sphinx_product_backend_tests)

    add_executable(sphinx_product_http_tests test/http/product_http_test.cpp)
    target_link_libraries(sphinx_product_http_tests PRIVATE sphinx_product_http ${GTEST_BOTH_LIBRARIES})
    add_test(NAME sphinx_product_http_tests COMMAND sphinx_product_http_tests)

    add_executable(sphinx_mysql_product_integration_tests
            test/backends/mysql/mysql_product_store_integration_test.cpp)
    target_link_libraries(sphinx_mysql_product_integration_tests PRIVATE sphinx_product_mysql
            PkgConfig::MYSQLCLIENT ${GTEST_BOTH_LIBRARIES})
    target_link_options(sphinx_mysql_product_integration_tests PRIVATE "-Wl,--wrap=mysql_commit")
    add_test(NAME sphinx_mysql_product_integration_tests COMMAND sphinx_mysql_product_integration_tests)
endif ()

find_package(Python3 COMPONENTS Interpreter QUIET)
if (Python3_Interpreter_FOUND)
    add_test(NAME sphinx_product_cache_comparison_tests
            COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/test/tools/compare_product_cache_test.py)
    add_test(NAME sphinx_product_http_integration_test
            COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/test/http/product_http_integration_test.py
            $<TARGET_FILE:sphinxd> $<TARGET_FILE:sphinx-product-service>)
endif ()
