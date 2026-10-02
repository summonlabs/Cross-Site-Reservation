cmake_minimum_required(VERSION 3.20)

project(CrossSiteReservation
        VERSION 1.0.0
        DESCRIPTION "Cross-Site Reservation authority runtime"
        HOMEPAGE_URL "https://github.com/summonlabs/Cross-Site-Reservation"
        LANGUAGES CXX)

option(XSR_BUILD_APPS "Build the coordinator, participant and CLI executables" ON)
option(XSR_BUILD_TESTS "Build the test suite" ON)
option(XSR_BUILD_BENCHMARKS "Build the benchmark executable" ON)
option(XSR_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" ON)
option(XSR_ENABLE_SANITIZERS "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(XSR_INSTALL "Generate install and export rules" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

function(xsr_strict_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
    if(XSR_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wcast-qual -Wnon-virtual-dtor
      -Wold-style-cast -Woverloaded-virtual -Wdouble-promotion)
    # -Wuseless-cast is deliberately not enabled: narrowing/widening casts between
    # size_t and the fixed-width integer types are required for portability and
    # the diagnostic rejects them on targets where the widths happen to match.
    if(XSR_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

function(xsr_sanitizers target)
  # PUBLIC rather than PRIVATE: a sanitized static library requires the final
  # executable to compile and link the same runtime, and that must hold for a
  # downstream consumer of the installed package as well.
  if(XSR_ENABLE_SANITIZERS)
    if(MSVC)
      target_compile_options(${target} PUBLIC /fsanitize=address)
    else()
      target_compile_options(${target} PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(${target} PUBLIC -fsanitize=address,undefined)
    endif()
  endif()
endfunction()

find_package(Threads REQUIRED)

add_library(xsr_runtime STATIC
  src/core/error.cpp
  src/core/digest.cpp
  src/core/canonical.cpp
  src/core/ids.cpp
  src/core/capacity.cpp
  src/core/clock.cpp
  src/core/random.cpp
  src/core/fault.cpp
  src/core/platform_files.cpp
  src/platform/socket.cpp
  src/store/record_store.cpp
  src/site/site_ledger.cpp
  src/site/participant.cpp
  src/protocol/records.cpp
  src/protocol/protocol.cpp
  src/wire/client.cpp
  src/coordinator/coordinator_state.cpp
  src/coordinator/coordinator_protocol.cpp)

add_library(Summon::xsr ALIAS xsr_runtime)

target_include_directories(xsr_runtime
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_features(xsr_runtime PUBLIC cxx_std_20)
target_link_libraries(xsr_runtime PUBLIC Threads::Threads)

if(WIN32)
  target_link_libraries(xsr_runtime PUBLIC ws2_32 bcrypt)
endif()

# EXPORT_NAME makes the installed target Summon::xsr, matching the build-tree
# alias, without renaming the internal target.
set_target_properties(xsr_runtime PROPERTIES
  OUTPUT_NAME xsr
  EXPORT_NAME xsr
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR})

xsr_strict_warnings(xsr_runtime)
xsr_sanitizers(xsr_runtime)

if(XSR_BUILD_APPS)
  add_executable(xsr_coordinator apps/coordinator_main.cpp)
  target_link_libraries(xsr_coordinator PRIVATE xsr_runtime)
  xsr_strict_warnings(xsr_coordinator)
  xsr_sanitizers(xsr_coordinator)

  add_executable(xsr_site apps/site_main.cpp)
  target_link_libraries(xsr_site PRIVATE xsr_runtime)
  xsr_strict_warnings(xsr_site)
  xsr_sanitizers(xsr_site)

  add_executable(xsr_cli apps/cli_main.cpp)
  target_link_libraries(xsr_cli PRIVATE xsr_runtime)
  xsr_strict_warnings(xsr_cli)
  xsr_sanitizers(xsr_cli)
  set_target_properties(xsr_cli PROPERTIES OUTPUT_NAME xsr)
endif()

if(XSR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(XSR_BUILD_BENCHMARKS)
  add_executable(xsr_bench bench/bench_main.cpp)
  target_link_libraries(xsr_bench PRIVATE xsr_runtime)
  xsr_strict_warnings(xsr_bench)
  xsr_sanitizers(xsr_bench)
endif()

if(XSR_INSTALL)
  install(TARGETS xsr_runtime
          EXPORT xsrTargets
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
          LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
          ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
          INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

  install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

  install(EXPORT xsrTargets
          FILE xsrTargets.cmake
          NAMESPACE Summon::
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/xsr)

  configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/xsrConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/xsrConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/xsr)

  write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/xsrConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

  install(FILES
          ${CMAKE_CURRENT_BINARY_DIR}/xsrConfig.cmake
          ${CMAKE_CURRENT_BINARY_DIR}/xsrConfigVersion.cmake
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/xsr)

  install(FILES LICENSE NOTICE README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
endif()
