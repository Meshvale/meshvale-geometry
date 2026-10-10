# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.24)
project(MeshvaleGeometry LANGUAGES CXX)
set(CMAKE_CXX_EXTENSIONS OFF)

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

include(cmake/MeshvaleEigen.cmake)
add_library(meshvale_geometry STATIC src/attributes.cpp src/mesh.cpp src/topology.cpp src/position_buffer.cpp)
add_library(meshvale::geometry ALIAS meshvale_geometry)
set_target_properties(meshvale_geometry PROPERTIES EXPORT_NAME geometry POSITION_INDEPENDENT_CODE ON)
target_compile_features(meshvale_geometry PUBLIC cxx_std_20)
# Resolve header usage privately rather than exporting a LINK_ONLY Eigen target
# from the static library. Public headers contain no Eigen types/templates.
target_include_directories(meshvale_geometry SYSTEM PRIVATE
  $<TARGET_PROPERTY:Eigen3::Eigen,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_definitions(meshvale_geometry PRIVATE
  EIGEN_MPL2_ONLY EIGEN_DONT_PARALLELIZE EIGEN_FAST_MATH=0)
target_include_directories(meshvale_geometry PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)

find_package(Threads REQUIRED)
add_library(meshvale_geometry_editing STATIC src/editable_mesh.cpp src/execution.cpp)
add_library(meshvale::editing ALIAS meshvale_geometry_editing)
set_target_properties(meshvale_geometry_editing PROPERTIES
  EXPORT_NAME editing POSITION_INDEPENDENT_CODE ON)
target_link_libraries(meshvale_geometry_editing PUBLIC meshvale::geometry Threads::Threads)
add_library(meshvale_geometry_triangulation STATIC src/triangulation.cpp)
add_library(meshvale::triangulation ALIAS meshvale_geometry_triangulation)
set_target_properties(meshvale_geometry_triangulation PROPERTIES
  EXPORT_NAME triangulation POSITION_INDEPENDENT_CODE ON)
target_link_libraries(meshvale_geometry_triangulation PUBLIC meshvale::editing)
if(MSVC)
  target_compile_options(meshvale_geometry PRIVATE /W4 /WX /fp:strict)
  target_compile_options(meshvale_geometry_editing PRIVATE /W4 /WX)
  target_compile_options(meshvale_geometry_triangulation PRIVATE /W4 /WX /fp:strict)
else()
  target_compile_options(meshvale_geometry PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-fast-math)
  target_compile_options(meshvale_geometry_editing PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(meshvale_geometry_triangulation PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-fast-math)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS meshvale_geometry EXPORT MeshvaleGeometryTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(TARGETS meshvale_geometry_editing EXPORT MeshvaleGeometryTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(TARGETS meshvale_geometry_triangulation EXPORT MeshvaleGeometryTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT MeshvaleGeometryTargets NAMESPACE meshvale::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/MeshvaleGeometry")
set(MeshvaleGeometry_RECORD_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/python/record.cpp")
include(cmake/MeshvaleGeometryPythonRecord.cmake)
install(FILES src/python/record.cpp DESTINATION "${CMAKE_INSTALL_DATADIR}/MeshvaleGeometry/python")
install(FILES cmake/MeshvaleGeometryPythonRecord.cmake
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/MeshvaleGeometry")
configure_package_config_file(cmake/MeshvaleGeometryConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/MeshvaleGeometryConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/MeshvaleGeometry"
  PATH_VARS CMAKE_INSTALL_DATADIR)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/MeshvaleGeometryConfigVersion.cmake"
  VERSION "${MESHVALE_GEOMETRY_VERSION}" COMPATIBILITY ExactVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/MeshvaleGeometryConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/MeshvaleGeometryConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/MeshvaleGeometry")
install(FILES LICENSE NOTICE DESTINATION "${CMAKE_INSTALL_DATADIR}/MeshvaleGeometry")
install(DIRECTORY licenses/ DESTINATION "${CMAKE_INSTALL_DATADIR}/MeshvaleGeometry/licenses")
install(FILES python/meshvale_reports/report-v1.schema.json
  DESTINATION "${CMAKE_INSTALL_DATADIR}/MeshvaleGeometry/schemas")

option(MESHVALE_GEOMETRY_PYTHON "Build the optional Python snapshot interface" OFF)
if(MESHVALE_GEOMETRY_PYTHON)
  find_package(Python 3.10 REQUIRED COMPONENTS Interpreter Development.Module)
  execute_process(COMMAND "${Python_EXECUTABLE}" -c
    "import sysconfig; print(int(bool(sysconfig.get_config_var('Py_GIL_DISABLED'))))"
    OUTPUT_VARIABLE meshvale_python_free_threaded OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
  if(meshvale_python_free_threaded STREQUAL "1")
    message(FATAL_ERROR "Free-threaded Python is not supported by this binding")
  endif()
  execute_process(COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
    OUTPUT_VARIABLE meshvale_nanobind_dir OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
  find_package(nanobind 3.1 CONFIG REQUIRED HINTS "${meshvale_nanobind_dir}")
  nanobind_add_module(_geometry NB_STATIC NB_DOMAIN meshvale_geometry
    python/bindings.cpp python/snapshot.cpp python/triangulation_bindings.cpp)
  meshvale_geometry_add_python_record(meshvale_geometry_record DOMAIN meshvale_geometry)
  set(meshvale_record_probe "${CMAKE_CURRENT_BINARY_DIR}/record_header_h.cpp")
  file(WRITE "${meshvale_record_probe}"
    "#include <meshvale/geometry/python/record.h>\n#include <meshvale/geometry/python/record.h>\n")
  target_sources(_geometry PRIVATE "${meshvale_record_probe}")
  target_include_directories(_geometry PRIVATE python)
  foreach(meshvale_private_header IN ITEMS snapshot triangulation_bindings)
    set(meshvale_private_probe "${CMAKE_CURRENT_BINARY_DIR}/${meshvale_private_header}_header.cpp")
    file(WRITE "${meshvale_private_probe}"
      "#include \"${meshvale_private_header}.h\"\n#include \"${meshvale_private_header}.h\"\n")
    target_sources(_geometry PRIVATE "${meshvale_private_probe}")
  endforeach()
  target_link_libraries(_geometry PRIVATE meshvale_geometry_record meshvale::triangulation)
  if(MSVC)
    target_compile_options(_geometry PRIVATE /W4 /WX)
  else()
    target_compile_options(_geometry PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  install(TARGETS _geometry LIBRARY DESTINATION meshvale_geometry COMPONENT python)
endif()

include(CTest)
if(BUILD_TESTING)
  option(MESHVALE_GEOMETRY_ALLOCATION_PROBE
    "Build ordinary replacement-new allocation failure tests (disable for TSan)" ON)
  add_library(meshvale_geometry_header_checks OBJECT)
  target_link_libraries(meshvale_geometry_header_checks PRIVATE meshvale::geometry)
  foreach(meshvale_header IN ITEMS attributes mesh topology execution position_buffer)
    set(meshvale_probe "${CMAKE_CURRENT_BINARY_DIR}/${meshvale_header}_h.cpp")
    file(WRITE "${meshvale_probe}"
      "#include <meshvale/geometry/${meshvale_header}.h>\n#include <meshvale/geometry/${meshvale_header}.h>\n")
    target_sources(meshvale_geometry_header_checks PRIVATE "${meshvale_probe}")
  endforeach()
  set(meshvale_editor_probe "${CMAKE_CURRENT_BINARY_DIR}/editable_mesh_h.cpp")
  file(WRITE "${meshvale_editor_probe}"
    "#include <meshvale/geometry/editable_mesh.h>\n#include <meshvale/geometry/editable_mesh.h>\n")
  target_sources(meshvale_geometry_header_checks PRIVATE "${meshvale_editor_probe}")
  set(meshvale_triangulation_probe "${CMAKE_CURRENT_BINARY_DIR}/triangulation_h.cpp")
  file(WRITE "${meshvale_triangulation_probe}"
    "#include <meshvale/geometry/triangulation.h>\n#include <meshvale/geometry/triangulation.h>\n")
  target_sources(meshvale_geometry_header_checks PRIVATE "${meshvale_triangulation_probe}")
  add_executable(meshvale_geometry_tests tests/storage.cpp)
  target_link_libraries(meshvale_geometry_tests PRIVATE meshvale::geometry)
  add_executable(meshvale_topology_tests tests/topology.cpp)
  target_link_libraries(meshvale_topology_tests PRIVATE meshvale::geometry)
  add_executable(meshvale_editing_tests tests/editing.cpp)
  target_link_libraries(meshvale_editing_tests PRIVATE meshvale::editing)
  add_executable(meshvale_execution_tests tests/execution.cpp)
  target_link_libraries(meshvale_execution_tests PRIVATE meshvale::triangulation)
  if(MESHVALE_GEOMETRY_ALLOCATION_PROBE)
    add_executable(meshvale_execution_allocation_tests
      tests/execution_allocation.cpp tests/allocation_failure.cpp)
    target_link_libraries(meshvale_execution_allocation_tests PRIVATE meshvale::editing)
    # Preserve the replacement allocation compilation boundary under CMake IPO.
    set_target_properties(meshvale_execution_allocation_tests PROPERTIES
      INTERPROCEDURAL_OPTIMIZATION FALSE)
    foreach(meshvale_test_config IN ITEMS Debug Release RelWithDebInfo MinSizeRel
        ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
      string(TOUPPER "${meshvale_test_config}" meshvale_test_config_upper)
      set_property(TARGET meshvale_execution_allocation_tests PROPERTY
        "INTERPROCEDURAL_OPTIMIZATION_${meshvale_test_config_upper}" FALSE)
    endforeach()
    if(MSVC)
      target_compile_options(meshvale_execution_allocation_tests PRIVATE /W4 /WX)
    else()
      target_compile_options(meshvale_execution_allocation_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME execution_allocation_failure COMMAND meshvale_execution_allocation_tests)
  endif()
  add_executable(meshvale_triangulation_tests tests/triangulation.cpp)
  target_include_directories(meshvale_triangulation_tests PRIVATE src)
  target_link_libraries(meshvale_triangulation_tests PRIVATE meshvale::triangulation)
  add_executable(meshvale_triangulation_fixture_driver tests/triangulation_fixture_driver.cpp)
  target_link_libraries(meshvale_triangulation_fixture_driver PRIVATE meshvale::triangulation)
  find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
  if(MSVC)
    target_compile_options(meshvale_geometry_header_checks PRIVATE /W4 /WX)
    target_compile_options(meshvale_geometry_tests PRIVATE /W4 /WX)
    target_compile_options(meshvale_topology_tests PRIVATE /W4 /WX)
    target_compile_options(meshvale_editing_tests PRIVATE /W4 /WX)
    target_compile_options(meshvale_execution_tests PRIVATE /W4 /WX)
    target_compile_options(meshvale_triangulation_tests PRIVATE /W4 /WX)
    target_compile_options(meshvale_triangulation_fixture_driver PRIVATE /W4 /WX)
  else()
    target_compile_options(meshvale_geometry_header_checks PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(meshvale_geometry_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(meshvale_topology_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(meshvale_editing_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(meshvale_execution_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(meshvale_triangulation_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(meshvale_triangulation_fixture_driver PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME attribute_storage COMMAND meshvale_geometry_tests)
  add_test(NAME polygon_topology COMMAND meshvale_topology_tests)
  add_test(NAME pooled_editing COMMAND meshvale_editing_tests)
  add_test(NAME shared_execution_leases COMMAND meshvale_execution_tests)
  add_test(NAME exact_polygon_triangulation COMMAND meshvale_triangulation_tests)
  add_test(NAME triangulation_fraction_oracle
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_triangulation_oracle.py"
      $<TARGET_FILE:meshvale_triangulation_fixture_driver>)
endif()
