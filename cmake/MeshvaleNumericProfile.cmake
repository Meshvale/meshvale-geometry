# SPDX-License-Identifier: Apache-2.0
# Private admission facts for the compiled numerical owner, not an allocator.
set(_MESHVALE_NUMERIC_PROFILE_UNSUPPORTED 0)
set(_MESHVALE_NUMERIC_PROFILE_NATIVE 1)
set(_MESHVALE_NUMERIC_PROFILE_ASAN_UBSAN 2)
set(_MESHVALE_NUMERIC_PROFILE_TSAN 3)
set(_MESHVALE_NUMERIC_PHASE_COMPILE "COMPILE")
set(_MESHVALE_NUMERIC_PHASE_LINK "LINK")

function(meshvale_numeric_classify_tokens result phase)
  if(NOT phase STREQUAL "${_MESHVALE_NUMERIC_PHASE_COMPILE}" AND
      NOT phase STREQUAL "${_MESHVALE_NUMERIC_PHASE_LINK}")
    set(${result} "${_MESHVALE_NUMERIC_PROFILE_UNSUPPORTED}" PARENT_SCOPE)
    return()
  endif()
  set(profile "${_MESHVALE_NUMERIC_PROFILE_NATIVE}")
  set(optimized FALSE)
  set(debug FALSE)
  set(frame_pointers FALSE)
  foreach(option IN LISTS ARGN)
    if(option STREQUAL "-fsanitize=address,undefined" OR
        option STREQUAL "-fsanitize=undefined,address")
      if(profile STREQUAL "${_MESHVALE_NUMERIC_PROFILE_TSAN}")
        set(${result} "${_MESHVALE_NUMERIC_PROFILE_UNSUPPORTED}" PARENT_SCOPE)
        return()
      endif()
      set(profile "${_MESHVALE_NUMERIC_PROFILE_ASAN_UBSAN}")
    elseif(option STREQUAL "-fsanitize=thread")
      if(profile STREQUAL "${_MESHVALE_NUMERIC_PROFILE_ASAN_UBSAN}")
        set(${result} "${_MESHVALE_NUMERIC_PROFILE_UNSUPPORTED}" PARENT_SCOPE)
        return()
      endif()
      set(profile "${_MESHVALE_NUMERIC_PROFILE_TSAN}")
    elseif(option STREQUAL "-O1" AND
        phase STREQUAL "${_MESHVALE_NUMERIC_PHASE_COMPILE}")
      set(optimized TRUE)
    elseif(option STREQUAL "-g")
      set(debug TRUE)
    elseif(option STREQUAL "-fno-omit-frame-pointer")
      set(frame_pointers TRUE)
    elseif(NOT option STREQUAL "-Wall" AND NOT option STREQUAL "-Wextra" AND
        NOT option STREQUAL "-Wpedantic" AND NOT option STREQUAL "-Werror" AND
        NOT option STREQUAL "-fno-fast-math")
      set(${result} "${_MESHVALE_NUMERIC_PROFILE_UNSUPPORTED}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  if((profile STREQUAL "${_MESHVALE_NUMERIC_PROFILE_TSAN}" AND
      phase STREQUAL "${_MESHVALE_NUMERIC_PHASE_COMPILE}" AND
      (NOT optimized OR NOT debug OR NOT frame_pointers)) OR
      (NOT profile STREQUAL "${_MESHVALE_NUMERIC_PROFILE_TSAN}" AND optimized))
    set(${result} "${_MESHVALE_NUMERIC_PROFILE_UNSUPPORTED}" PARENT_SCOPE)
    return()
  endif()
  set(${result} "${profile}" PARENT_SCOPE)
endfunction()

function(meshvale_numeric_definitions_supported result)
  foreach(definition IN LISTS ARGN)
    if(NOT definition STREQUAL "EIGEN_MPL2_ONLY" AND
        NOT definition STREQUAL "EIGEN_DONT_PARALLELIZE" AND
        NOT definition STREQUAL "EIGEN_FAST_MATH=0" AND
        NOT definition STREQUAL
          "MESHVALE_GEOMETRY_NUMERIC_DEBUG_BUILD=$<IF:$<CONFIG:Debug>,1,0>")
      set(${result} FALSE PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set(${result} TRUE PARENT_SCOPE)
endfunction()

function(meshvale_geometry_classify_numeric_build)
  set(profile "${_MESHVALE_NUMERIC_PROFILE_UNSUPPORTED}")
  set(supported TRUE)
  # A parent could add options after a subdirectory's deferred callback. Only
  # the reviewed standalone owner can certify the final option set here.
  if(NOT PROJECT_IS_TOP_LEVEL OR
      NOT CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(supported FALSE)
  endif()
  foreach(input IN ITEMS CMAKE_CXX_COMPILER_ARG1 CMAKE_TOOLCHAIN_FILE
      CMAKE_CXX_COMPILER_LAUNCHER CMAKE_CXX_LINKER_LAUNCHER
      CMAKE_PROJECT_INCLUDE CMAKE_PROJECT_INCLUDE_BEFORE
      CMAKE_PROJECT_MeshvaleGeometry_INCLUDE
      CMAKE_PROJECT_MeshvaleGeometry_INCLUDE_BEFORE
      CMAKE_USER_MAKE_RULES_OVERRIDE CMAKE_USER_MAKE_RULES_OVERRIDE_CXX
      CMAKE_PROJECT_TOP_LEVEL_INCLUDES)
    if(NOT "${${input}}" STREQUAL "")
      set(supported FALSE)
    endif()
  endforeach()
  foreach(property IN ITEMS CXX_COMPILER_LAUNCHER CXX_LINKER_LAUNCHER
      RULE_LAUNCH_COMPILE RULE_LAUNCH_LINK LINK_LIBRARIES)
    get_property(value TARGET meshvale_geometry PROPERTY "${property}")
    if(NOT "${value}" STREQUAL "")
      set(supported FALSE)
    endif()
  endforeach()
  foreach(property IN ITEMS RULE_LAUNCH_COMPILE RULE_LAUNCH_LINK)
    get_property(directory_value DIRECTORY PROPERTY "${property}")
    get_property(global_value GLOBAL PROPERTY "${property}")
    if(NOT "${directory_value}" STREQUAL "" OR NOT "${global_value}" STREQUAL "")
      set(supported FALSE)
    endif()
  endforeach()
  foreach(property IN ITEMS INTERPROCEDURAL_OPTIMIZATION
      INTERPROCEDURAL_OPTIMIZATION_DEBUG UNITY_BUILD)
    get_property(value TARGET meshvale_geometry PROPERTY "${property}")
    if(value)
      set(supported FALSE)
    endif()
  endforeach()
  get_property(standard TARGET meshvale_geometry PROPERTY CXX_STANDARD)
  get_property(extensions TARGET meshvale_geometry PROPERTY CXX_EXTENSIONS)
  if((NOT "${standard}" STREQUAL "" AND NOT "${standard}" STREQUAL "20") OR extensions)
    set(supported FALSE)
  endif()

  separate_arguments(global_options UNIX_COMMAND
    "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_DEBUG}")
  get_property(target_options TARGET meshvale_geometry PROPERTY COMPILE_OPTIONS)
  get_property(directory_options DIRECTORY PROPERTY COMPILE_OPTIONS)
  get_property(target_flags TARGET meshvale_geometry PROPERTY COMPILE_FLAGS)
  separate_arguments(legacy_options UNIX_COMMAND "${target_flags}")
  set(common_options ${global_options} ${target_options}
    ${directory_options} ${legacy_options})
  get_property(target_definitions TARGET meshvale_geometry PROPERTY COMPILE_DEFINITIONS)
  get_property(directory_definitions DIRECTORY PROPERTY COMPILE_DEFINITIONS)
  get_property(target_debug_definitions TARGET meshvale_geometry PROPERTY COMPILE_DEFINITIONS_DEBUG)
  get_property(directory_debug_definitions DIRECTORY PROPERTY COMPILE_DEFINITIONS_DEBUG)
  set(common_definitions ${target_definitions} ${directory_definitions}
    ${target_debug_definitions} ${directory_debug_definitions})

  set(previous_profile "")
  foreach(owner IN ITEMS src/scalar_buffer.cpp src/vector_buffer.cpp)
    get_source_file_property(source_options "${owner}" COMPILE_OPTIONS)
    get_source_file_property(source_flags "${owner}" COMPILE_FLAGS)
    get_source_file_property(source_definitions "${owner}" COMPILE_DEFINITIONS)
    get_source_file_property(debug_definitions "${owner}" COMPILE_DEFINITIONS_DEBUG)
    foreach(variable IN ITEMS source_options source_flags source_definitions debug_definitions)
      if("${${variable}}" STREQUAL "NOTFOUND")
        set(${variable} "")
      endif()
    endforeach()
    separate_arguments(source_legacy UNIX_COMMAND "${source_flags}")
    meshvale_numeric_classify_tokens(owner_profile
      "${_MESHVALE_NUMERIC_PHASE_COMPILE}" ${common_options}
      ${source_options} ${source_legacy})
    meshvale_numeric_definitions_supported(definitions_supported
      ${common_definitions} ${source_definitions} ${debug_definitions})
    if(owner_profile STREQUAL "${_MESHVALE_NUMERIC_PROFILE_UNSUPPORTED}" OR
        NOT definitions_supported OR
        (NOT previous_profile STREQUAL "" AND NOT previous_profile STREQUAL owner_profile))
      set(supported FALSE)
    endif()
    set(previous_profile "${owner_profile}")
  endforeach()

  separate_arguments(global_link_options UNIX_COMMAND
    "${CMAKE_EXE_LINKER_FLAGS} ${CMAKE_EXE_LINKER_FLAGS_DEBUG} ${CMAKE_STATIC_LINKER_FLAGS} ${CMAKE_STATIC_LINKER_FLAGS_DEBUG}")
  get_property(target_link_options TARGET meshvale_geometry PROPERTY LINK_OPTIONS)
  get_property(directory_link_options DIRECTORY PROPERTY LINK_OPTIONS)
  get_property(static_options TARGET meshvale_geometry PROPERTY STATIC_LIBRARY_OPTIONS)
  get_property(target_link_flags TARGET meshvale_geometry PROPERTY LINK_FLAGS)
  get_property(target_debug_link_flags TARGET meshvale_geometry PROPERTY LINK_FLAGS_DEBUG)
  get_property(static_flags TARGET meshvale_geometry PROPERTY STATIC_LIBRARY_FLAGS)
  separate_arguments(legacy_link_options UNIX_COMMAND
    "${target_link_flags} ${target_debug_link_flags} ${static_flags}")
  meshvale_numeric_classify_tokens(link_profile
    "${_MESHVALE_NUMERIC_PHASE_LINK}" ${global_link_options}
    ${target_link_options} ${directory_link_options} ${static_options}
    ${legacy_link_options})
  if(link_profile STREQUAL "${_MESHVALE_NUMERIC_PROFILE_UNSUPPORTED}" OR
      (link_profile STREQUAL "${_MESHVALE_NUMERIC_PROFILE_ASAN_UBSAN}" AND
        NOT previous_profile STREQUAL link_profile) OR
      (link_profile STREQUAL "${_MESHVALE_NUMERIC_PROFILE_TSAN}" AND
        NOT previous_profile STREQUAL link_profile) OR
      (previous_profile STREQUAL "${_MESHVALE_NUMERIC_PROFILE_TSAN}" AND
        NOT link_profile STREQUAL previous_profile))
    set(supported FALSE)
  endif()
  if(supported)
    set(profile "${previous_profile}")
  endif()
  target_compile_definitions(meshvale_geometry PRIVATE
    "MESHVALE_GEOMETRY_NUMERIC_INSTRUMENTATION=$<IF:$<CONFIG:Debug>,${profile},${_MESHVALE_NUMERIC_PROFILE_UNSUPPORTED}>")
endfunction()
