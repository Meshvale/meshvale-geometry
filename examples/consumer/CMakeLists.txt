# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.24)
project(MeshvaleInstalledConsumer LANGUAGES CXX)
find_package(MeshvaleGeometry CONFIG REQUIRED)
add_executable(meshvale_consumer main.cpp)
target_link_libraries(meshvale_consumer PRIVATE meshvale::geometry)
enable_testing()
add_test(NAME installed_consumer COMMAND meshvale_consumer)
