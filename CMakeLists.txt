# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.24)
project(MeshvaleGeometry LANGUAGES CXX)

# Exact release tags own released versions; untagged builds are development snapshots.
find_package(Git QUIET)
set(MESHVALE_GEOMETRY_VERSION "0.0.0")
if(Git_FOUND)
  execute_process(COMMAND "${GIT_EXECUTABLE}" describe --tags --exact-match --dirty
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE meshvale_tag OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(meshvale_tag MATCHES "^v([0-9]+\\.[0-9]+\\.[0-9]+)$")
    set(MESHVALE_GEOMETRY_VERSION "${CMAKE_MATCH_1}")
  endif()
endif()

add_library(meshvale_geometry INTERFACE)
add_library(meshvale::geometry ALIAS meshvale_geometry)
set_target_properties(meshvale_geometry PROPERTIES EXPORT_NAME geometry)
target_compile_features(meshvale_geometry INTERFACE cxx_std_20)
target_include_directories(meshvale_geometry INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS meshvale_geometry EXPORT MeshvaleGeometryTargets)
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT MeshvaleGeometryTargets NAMESPACE meshvale::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/MeshvaleGeometry")
configure_package_config_file(cmake/MeshvaleGeometryConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/MeshvaleGeometryConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/MeshvaleGeometry")
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/MeshvaleGeometryConfigVersion.cmake"
  VERSION "${MESHVALE_GEOMETRY_VERSION}" COMPATIBILITY ExactVersion ARCH_INDEPENDENT)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/MeshvaleGeometryConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/MeshvaleGeometryConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/MeshvaleGeometry")
install(FILES LICENSE NOTICE DESTINATION "${CMAKE_INSTALL_DATADIR}/MeshvaleGeometry")

include(CTest)
if(BUILD_TESTING)
  add_executable(meshvale_geometry_tests tests/storage.cpp)
  target_link_libraries(meshvale_geometry_tests PRIVATE meshvale::geometry)
  if(MSVC)
    target_compile_options(meshvale_geometry_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(meshvale_geometry_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME attribute_storage COMMAND meshvale_geometry_tests)
endif()
