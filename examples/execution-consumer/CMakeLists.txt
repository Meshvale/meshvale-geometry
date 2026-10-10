# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.24)
project(MeshvaleInstalledExecutionConsumer LANGUAGES CXX)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(MeshvaleGeometry CONFIG REQUIRED)
add_executable(meshvale_execution_consumer main.cpp)
target_link_libraries(meshvale_execution_consumer PRIVATE meshvale::editing)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/execution_header.cpp"
  "#include <meshvale/geometry/execution.h>\n#include <meshvale/geometry/execution.h>\n")
target_sources(meshvale_execution_consumer PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}/execution_header.cpp")
if(MSVC)
  target_compile_options(meshvale_execution_consumer PRIVATE /W4 /WX)
else()
  target_compile_options(meshvale_execution_consumer PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
endif()
enable_testing()
add_test(NAME installed_execution_consumer COMMAND meshvale_execution_consumer)
