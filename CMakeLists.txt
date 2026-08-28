cmake_minimum_required(VERSION 3.8 FATAL_ERROR)

project(Sphinx LANGUAGES CXX)

enable_testing()

find_program(RAGEL_EXECUTABLE NAMES ragel)
if(NOT RAGEL_EXECUTABLE)
  message(FATAL_ERROR "Ragel is required to build Sphinx")
endif()
find_package(GTest)

set(SPHINX_VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address -fno-omit-frame-pointer")

set(CMAKE_CXX_FLAGS "-O3 -g -Wall -Wextra")

execute_process(
  COMMAND git log -1 --format=%h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE GIT_COMMIT_HASH
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

add_subdirectory(sphinxd)
