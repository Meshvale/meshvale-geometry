# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.24)
project(MeshvaleInstalledEditingConsumer LANGUAGES CXX)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(MeshvaleGeometry CONFIG REQUIRED)
add_executable(meshvale_editing_consumer main.cpp)
target_link_libraries(meshvale_editing_consumer PRIVATE meshvale::editing)
# An otherwise empty translation unit catches a header that relies on another
# project header or the consumer's standard-library includes.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/editable_mesh_header.cpp"
  "#include <meshvale/geometry/editable_mesh.h>\n#include <meshvale/geometry/editable_mesh.h>\n")
target_sources(meshvale_editing_consumer PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}/editable_mesh_header.cpp")
if(MSVC)
  target_compile_options(meshvale_editing_consumer PRIVATE /W4 /WX)
else()
  target_compile_options(meshvale_editing_consumer PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
endif()
enable_testing()
add_test(NAME installed_editing_consumer COMMAND meshvale_editing_consumer)
