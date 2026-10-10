# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.24)
project(MeshvaleInstalledTriangulationConsumer LANGUAGES CXX)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(MeshvaleGeometry CONFIG REQUIRED)
add_executable(meshvale_triangulation_consumer main.cpp)
target_link_libraries(meshvale_triangulation_consumer PRIVATE meshvale::triangulation)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/triangulation_header.cpp"
  "#include <meshvale/geometry/triangulation.h>\n#include <meshvale/geometry/triangulation.h>\n")
target_sources(meshvale_triangulation_consumer PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}/triangulation_header.cpp")
if(MSVC)
  target_compile_options(meshvale_triangulation_consumer PRIVATE /W4 /WX)
else()
  target_compile_options(meshvale_triangulation_consumer PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
endif()
enable_testing()
add_test(NAME installed_triangulation_consumer COMMAND meshvale_triangulation_consumer)
