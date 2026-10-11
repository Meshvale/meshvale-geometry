# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.24)
project(MeshvaleLinuxNumericRequests LANGUAGES CXX)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR "The request observer requires Linux Clang original-route delegation")
endif()
if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
  message(FATAL_ERROR "Use a matching Debug Geometry and observer cohort")
endif()
set(OBSERVER_PROFILE "native" CACHE STRING "Matching native or asan-ubsan Geometry profile")
set(OBSERVER_GEOMETRY_SOURCE "" CACHE PATH "Exact source paired with the installed Geometry libraries")
set(OBSERVER_GEOMETRY_REVISION "" CACHE STRING "Exact forty-character Geometry commit")
string(LENGTH "${OBSERVER_GEOMETRY_REVISION}" revision_length)
if(NOT revision_length EQUAL 40 OR NOT OBSERVER_GEOMETRY_REVISION MATCHES "^[0-9a-f]+$")
  message(FATAL_ERROR "Supply an exact Geometry source revision")
endif()
foreach(owner IN ITEMS scalar_buffer vector_buffer materialization_allocation)
  if(NOT EXISTS "${OBSERVER_GEOMETRY_SOURCE}/src/${owner}.cpp")
    message(FATAL_ERROR "The exact Geometry source owner is missing: ${owner}")
  endif()
  file(SHA256 "${OBSERVER_GEOMETRY_SOURCE}/src/${owner}.cpp" "${owner}_hash")
endforeach()
find_package(MeshvaleGeometry CONFIG REQUIRED)
add_executable(observer observer.cpp trace.cpp wrappers.cpp)
target_compile_features(observer PRIVATE cxx_std_20)
set_target_properties(observer PROPERTIES CXX_EXTENSIONS OFF INTERPROCEDURAL_OPTIMIZATION FALSE)
target_link_libraries(observer PRIVATE meshvale::materialization)
target_include_directories(observer PRIVATE "${OBSERVER_GEOMETRY_SOURCE}/src")
target_compile_options(observer PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-fast-math -fno-builtin -fno-lto)
target_link_options(observer PRIVATE -fno-lto "LINKER:-Map,${CMAKE_CURRENT_BINARY_DIR}/observer.map")
target_compile_definitions(observer PRIVATE
  OBSERVER_PROFILE="${OBSERVER_PROFILE}"
  OBSERVER_GEOMETRY_REVISION="${OBSERVER_GEOMETRY_REVISION}"
  OBSERVER_SCALAR_SOURCE_HASH="${scalar_buffer_hash}"
  OBSERVER_VECTOR_SOURCE_HASH="${vector_buffer_hash}"
  OBSERVER_ALLOCATION_SOURCE_HASH="${materialization_allocation_hash}")
if(OBSERVER_PROFILE STREQUAL "asan-ubsan")
  target_compile_options(observer PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(observer PRIVATE -fsanitize=address,undefined)
elseif(NOT OBSERVER_PROFILE STREQUAL "native")
  message(FATAL_ERROR "Request interception supports only native and matching ASan/UBSan; use separate real-worker TSan tests")
endif()
foreach(symbol IN ITEMS malloc free calloc realloc _Znwm _Znam
    _ZnwmSt11align_val_t _ZnamSt11align_val_t _ZdlPv _ZdaPv _ZdlPvm _ZdaPvm
    _ZdlPvSt11align_val_t _ZdaPvSt11align_val_t
    _ZdlPvmSt11align_val_t _ZdaPvmSt11align_val_t)
  target_link_options(observer PRIVATE "LINKER:--wrap=${symbol}")
endforeach()
