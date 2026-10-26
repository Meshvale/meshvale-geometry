# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.24)
project(MeshvaleInstalledChargedTriangulationConsumer LANGUAGES CXX)
set(CMAKE_CXX_EXTENSIONS OFF)
set(MESHVALE_CHARGED_EXPECTED_PROFILE "qualified" CACHE STRING
  "Expected actual producer allocation profile: qualified or unsupported")
if(NOT MESHVALE_CHARGED_EXPECTED_PROFILE STREQUAL "qualified" AND
   NOT MESHVALE_CHARGED_EXPECTED_PROFILE STREQUAL "unsupported")
  message(FATAL_ERROR "Unknown charged triangulation expected profile")
endif()
find_package(MeshvaleGeometry CONFIG REQUIRED)
add_executable(meshvale_charged_triangulation_consumer main.cpp)
target_link_libraries(meshvale_charged_triangulation_consumer PRIVATE meshvale::charged_triangulation)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/charged_header.cpp"
  "#include <meshvale/geometry/charged_triangulation.h>\n#include <meshvale/geometry/charged_triangulation.h>\n")
target_sources(meshvale_charged_triangulation_consumer PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}/charged_header.cpp")
if(MSVC)
  target_compile_options(meshvale_charged_triangulation_consumer PRIVATE /W4 /WX /fp:strict)
else()
  target_compile_options(meshvale_charged_triangulation_consumer PRIVATE
    -Wall -Wextra -Wpedantic -Werror -fno-fast-math)
endif()
enable_testing()
if(MESHVALE_CHARGED_EXPECTED_PROFILE STREQUAL "qualified")
  add_test(NAME installed_charged_triangulation_consumer COMMAND meshvale_charged_triangulation_consumer)
else()
  add_test(NAME installed_charged_triangulation_consumer COMMAND meshvale_charged_triangulation_consumer unsupported)
endif()
