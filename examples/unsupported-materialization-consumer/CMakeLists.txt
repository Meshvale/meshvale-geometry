# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.24)
project(MeshvaleInstalledUnsupportedMaterializationConsumer LANGUAGES CXX)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(MeshvaleGeometry CONFIG REQUIRED)
# Exercise the same public contract against the installed library, without
# depending on implementation headers or producer CMake targets.
add_executable(meshvale_unsupported_materialization_consumer
  ../../tests/unsupported_materialization_profile.cpp)
target_link_libraries(meshvale_unsupported_materialization_consumer PRIVATE
  meshvale::materialization)
if(MSVC)
  target_compile_options(meshvale_unsupported_materialization_consumer PRIVATE
    /W4 /WX /fp:strict)
else()
  target_compile_options(meshvale_unsupported_materialization_consumer PRIVATE
    -Wall -Wextra -Wpedantic -Werror -fno-fast-math)
endif()
enable_testing()
add_test(NAME installed_unsupported_materialization_profile
  COMMAND meshvale_unsupported_materialization_consumer)
