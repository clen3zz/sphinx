cmake_minimum_required(VERSION 3.16 FATAL_ERROR)

project(Sphinx LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 自动检测并启用 ccache
find_program(CCACHE_PROGRAM ccache)
if (CCACHE_PROGRAM)
    message(STATUS "Found ccache: ${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
endif ()

enable_testing()

find_package(GTest)

set(SPHINX_VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif ()

option(ENABLE_ASAN "Enable AddressSanitizer" OFF)

add_compile_options(-Wall -Wextra)
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g")
if (ENABLE_ASAN)
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address -fno-omit-frame-pointer")
endif ()
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -g -DNDEBUG")

execute_process(
        COMMAND git log -1 --format=%h
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_COMMIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
)

add_subdirectory(sphinxd)
