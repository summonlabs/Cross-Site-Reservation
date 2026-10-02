# Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
# Installs the project and builds an independent consumer against the installed
# tree only. Never configures the consumer against the producer build tree.

if(NOT DEFINED XSR_SOURCE_DIR OR NOT DEFINED XSR_BINARY_DIR OR NOT DEFINED XSR_VALIDATION_ROOT)
  message(FATAL_ERROR "XSR_SOURCE_DIR, XSR_BINARY_DIR and XSR_VALIDATION_ROOT are required")
endif()

set(prefix "${XSR_VALIDATION_ROOT}/prefix")
set(consumer_build "${XSR_VALIDATION_ROOT}/consumer-build")
file(REMOVE_RECURSE "${XSR_VALIDATION_ROOT}")
file(MAKE_DIRECTORY "${XSR_VALIDATION_ROOT}")

set(install_command ${CMAKE_COMMAND} --install "${XSR_BINARY_DIR}" --prefix "${prefix}")
if(DEFINED XSR_BUILD_TYPE AND NOT XSR_BUILD_TYPE STREQUAL "")
  list(APPEND install_command --config "${XSR_BUILD_TYPE}")
endif()
execute_process(COMMAND ${install_command}
                RESULT_VARIABLE install_result
                OUTPUT_VARIABLE install_output
                ERROR_VARIABLE install_error)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "install failed (${install_result}): ${install_output} ${install_error}")
endif()

foreach(required IN ITEMS
    "include/xsr/coordinator.hpp"
    "include/xsr/ledger.hpp"
    "include/xsr/version.hpp"
    "include/xsr/store.hpp"
    "include/xsr/client.hpp")
  if(NOT EXISTS "${prefix}/${required}")
    message(FATAL_ERROR "installed package is missing ${required}")
  endif()
endforeach()

# Licence and notice are installed under the documentation directory.
file(GLOB_RECURSE installed_license "${prefix}/LICENSE" "${prefix}/*/LICENSE" "${prefix}/*/*/LICENSE")
file(GLOB_RECURSE installed_notice "${prefix}/NOTICE" "${prefix}/*/NOTICE" "${prefix}/*/*/NOTICE")
if(NOT installed_license)
  message(FATAL_ERROR "installed package is missing the LICENSE file")
endif()
if(NOT installed_notice)
  message(FATAL_ERROR "installed package is missing the NOTICE file")
endif()

file(GLOB xsr_config_files "${prefix}/lib/cmake/xsr/xsrConfig.cmake"
     "${prefix}/lib64/cmake/xsr/xsrConfig.cmake"
     "${prefix}/share/cmake/xsr/xsrConfig.cmake")
if(NOT xsr_config_files)
  message(FATAL_ERROR "no xsrConfig.cmake was installed under ${prefix}")
endif()

set(configure_command ${CMAKE_COMMAND}
                      -S "${XSR_SOURCE_DIR}/tests/package/consumer"
                      -B "${consumer_build}"
                      "-DCMAKE_PREFIX_PATH=${prefix}")
if(DEFINED XSR_GENERATOR AND NOT XSR_GENERATOR STREQUAL "")
  list(APPEND configure_command -G "${XSR_GENERATOR}")
endif()
if(DEFINED XSR_CXX_COMPILER AND NOT XSR_CXX_COMPILER STREQUAL "" AND NOT XSR_GENERATOR MATCHES "Visual Studio")
  list(APPEND configure_command "-DCMAKE_CXX_COMPILER=${XSR_CXX_COMPILER}")
endif()
if(DEFINED XSR_BUILD_TYPE AND NOT XSR_BUILD_TYPE STREQUAL "")
  list(APPEND configure_command "-DCMAKE_BUILD_TYPE=${XSR_BUILD_TYPE}")
endif()
execute_process(COMMAND ${configure_command}
                RESULT_VARIABLE configure_result
                OUTPUT_VARIABLE configure_output
                ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR "consumer configure failed: ${configure_output} ${configure_error}")
endif()

set(build_command ${CMAKE_COMMAND} --build "${consumer_build}")
if(DEFINED XSR_BUILD_TYPE AND NOT XSR_BUILD_TYPE STREQUAL "")
  list(APPEND build_command --config "${XSR_BUILD_TYPE}")
endif()
execute_process(COMMAND ${build_command}
                RESULT_VARIABLE build_result
                OUTPUT_VARIABLE build_output
                ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "consumer build failed: ${build_output} ${build_error}")
endif()

file(GLOB_RECURSE consumer_binaries "${consumer_build}/xsr_consumer*")
set(consumer_binary "")
foreach(candidate IN LISTS consumer_binaries)
  if(candidate MATCHES "\\.(exe)$" OR NOT candidate MATCHES "\\.(cmake|txt|ninja|obj|o|pdb|ilk|tlog|json)$")
    if(NOT IS_DIRECTORY "${candidate}")
      set(consumer_binary "${candidate}")
      break()
    endif()
  endif()
endforeach()
if(consumer_binary STREQUAL "")
  message(FATAL_ERROR "consumer executable was not produced under ${consumer_build}")
endif()

execute_process(COMMAND "${consumer_binary}" "${XSR_VALIDATION_ROOT}/consumer-state"
                RESULT_VARIABLE run_result
                OUTPUT_VARIABLE run_output
                ERROR_VARIABLE run_error)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "consumer run failed (${run_result}): ${run_output} ${run_error}")
endif()
if(NOT run_output MATCHES "xsr consumer ok")
  message(FATAL_ERROR "consumer did not report success: ${run_output}")
endif()

message(STATUS "package validation ok")
message(STATUS "${run_output}")
