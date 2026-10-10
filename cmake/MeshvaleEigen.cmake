# SPDX-License-Identifier: Apache-2.0
# Eigen is private header-only implementation; installed Geometry consumers need
# its compiled static library, not Eigen headers or an exported Eigen target.
find_package(Eigen3 3.4.1 EXACT QUIET NO_MODULE)
option(MESHVALE_GEOMETRY_FETCH_EIGEN "Fetch pinned Eigen if no exact installed package exists" ON)
if(NOT Eigen3_FOUND)
  if(NOT MESHVALE_GEOMETRY_FETCH_EIGEN)
    message(FATAL_ERROR "Eigen 3.4.1 CONFIG is required; set Eigen3_DIR or enable MESHVALE_GEOMETRY_FETCH_EIGEN")
  endif()
  include(FetchContent)
  FetchContent_Declare(meshvale_eigen
    URL https://gitlab.com/libeigen/eigen/-/archive/3.4.1/eigen-3.4.1.tar.gz
    URL_HASH SHA256=b93c667d1b69265cdb4d9f30ec21f8facbbe8b307cf34c0b9942834c6d4fdbe2
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    SOURCE_SUBDIR meshvale-no-cmake-project)
  FetchContent_MakeAvailable(meshvale_eigen)
  add_library(meshvale_eigen_headers INTERFACE)
  target_include_directories(meshvale_eigen_headers INTERFACE "${meshvale_eigen_SOURCE_DIR}")
  add_library(Eigen3::Eigen ALIAS meshvale_eigen_headers)
endif()
