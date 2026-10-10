# SPDX-License-Identifier: Apache-2.0
# Opt-in implementation-source factory. No Python discovery occurs on inclusion.
function(meshvale_geometry_add_python_record target)
  cmake_parse_arguments(PARSE_ARGV 1 record "" "DOMAIN" "")
  if(record_UNPARSED_ARGUMENTS OR record_KEYWORDS_MISSING_VALUES OR
     NOT record_DOMAIN MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
    message(FATAL_ERROR "Python record target requires DOMAIN with a C identifier")
  endif()
  if(NOT TARGET Python::Module OR NOT TARGET nanobind-static)
    message(FATAL_ERROR "Create an ordinary-GIL nanobind NB_STATIC module before its Python record target")
  endif()
  execute_process(COMMAND "${Python_EXECUTABLE}" -c
    "import sysconfig; print(int(bool(sysconfig.get_config_var('Py_GIL_DISABLED'))))"
    OUTPUT_VARIABLE record_free_threaded OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
  if(record_free_threaded STREQUAL "1")
    message(FATAL_ERROR "Free-threaded Python records are not supported")
  endif()
  add_library(${target} STATIC "${MeshvaleGeometry_RECORD_SOURCE}")
  set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON
    CXX_EXTENSIONS OFF CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
  target_compile_features(${target} PUBLIC cxx_std_20)
  target_compile_definitions(${target} PUBLIC "NB_DOMAIN=${record_DOMAIN}")
  target_link_libraries(${target} PUBLIC meshvale::geometry nanobind-static)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endfunction()
