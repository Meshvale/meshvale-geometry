# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.24)
project(MeshvaleInstalledMaterializationConsumer LANGUAGES CXX)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(MeshvaleGeometry CONFIG REQUIRED)
add_executable(meshvale_materialization_consumer main.cpp)
target_link_libraries(meshvale_materialization_consumer PRIVATE meshvale::materialization)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/materialization_header.cpp"
  "#include <meshvale/geometry/materialization.h>\n#include <meshvale/geometry/materialization.h>\n")
target_sources(meshvale_materialization_consumer PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}/materialization_header.cpp")
if(MSVC)
  target_compile_options(meshvale_materialization_consumer PRIVATE /W4 /WX /fp:strict)
else()
  target_compile_options(meshvale_materialization_consumer PRIVATE
    -Wall -Wextra -Wpedantic -Werror -fno-fast-math)
endif()
enable_testing()
add_test(NAME installed_materialization_consumer COMMAND meshvale_materialization_consumer)
