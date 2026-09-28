add_library(sphinx_product_core STATIC
        src/product_codec.cpp
        src/product_service.cpp
        src/sphinx_product_cache.cpp
)
target_include_directories(sphinx_product_core PUBLIC include)
target_link_libraries(sphinx_product_core PUBLIC sphinx_core)

if (GTEST_FOUND)
    add_executable(sphinx_product_tests
            test/product_codec_test.cpp
            test/product_service_test.cpp
    )
    target_link_libraries(sphinx_product_tests PRIVATE sphinx_product_core ${GTEST_BOTH_LIBRARIES} pthread)
    add_test(NAME sphinx_product_tests COMMAND sphinx_product_tests)
endif ()

# The normal Sphinx build has no MySQL/HTTP dependencies and does not download anything.
option(BUILD_MYSQL_SPHINX_DEMO "Build the optional MySQL product HTTP service" OFF)
if (BUILD_MYSQL_SPHINX_DEMO)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(MYSQLCLIENT REQUIRED IMPORTED_TARGET mysqlclient)
    include(FetchContent)

    FetchContent_Declare(cpp_httplib
            GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
            GIT_TAG v0.56.0
            GIT_SHALLOW TRUE)
    FetchContent_GetProperties(cpp_httplib)
    if (NOT cpp_httplib_POPULATED)
        FetchContent_Populate(cpp_httplib)
    endif ()
    add_library(sphinx_httplib INTERFACE)
    target_include_directories(sphinx_httplib INTERFACE ${cpp_httplib_SOURCE_DIR})

    FetchContent_Declare(nlohmann_json
            GIT_REPOSITORY https://github.com/nlohmann/json.git
            GIT_TAG v3.12.0
            GIT_SHALLOW TRUE)
    FetchContent_GetProperties(nlohmann_json)
    if (NOT nlohmann_json_POPULATED)
        FetchContent_Populate(nlohmann_json)
    endif ()
    add_library(sphinx_json INTERFACE)
    target_include_directories(sphinx_json INTERFACE ${nlohmann_json_SOURCE_DIR}/include)

    add_executable(sphinx-product-service
            src/mysql_product_store.cpp
            src/product_http.cpp
            src/product_http_routes.cpp
            src/product_main.cpp
    )
    target_link_libraries(sphinx-product-service PRIVATE sphinx_product_core PkgConfig::MYSQLCLIENT
            sphinx_httplib sphinx_json pthread)

    if (GTEST_FOUND)
        add_executable(sphinx_mysql_product_integration_tests
                test/mysql_product_store_integration_test.cpp
                src/mysql_product_store.cpp)
        target_link_libraries(sphinx_mysql_product_integration_tests PRIVATE sphinx_product_core
                PkgConfig::MYSQLCLIENT ${GTEST_BOTH_LIBRARIES} pthread)
        target_link_options(sphinx_mysql_product_integration_tests PRIVATE
                "-Wl,--wrap=mysql_commit")
        add_test(NAME sphinx_mysql_product_integration_tests
                COMMAND sphinx_mysql_product_integration_tests)
    endif ()

    if (Python3_Interpreter_FOUND)
        add_test(NAME sphinx_product_http_integration_test
                COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/product_http_integration_test.py
                $<TARGET_FILE:sphinxd> $<TARGET_FILE:sphinx-product-service>)
    endif ()
endif ()
