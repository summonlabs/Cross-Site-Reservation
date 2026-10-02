add_library(xsr_test_support STATIC
  framework.cpp
  support/process.cpp
  support/harness.cpp)

target_include_directories(xsr_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xsr_test_support PUBLIC xsr_runtime)
xsr_strict_warnings(xsr_test_support)
xsr_sanitizers(xsr_test_support)

function(xsr_add_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE xsr_test_support)
  xsr_strict_warnings(${name})
  xsr_sanitizers(${name})
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES LABELS "xsr")
endfunction()

xsr_add_test(xsr_test_core test_core.cpp)
xsr_add_test(xsr_test_store test_store.cpp)
xsr_add_test(xsr_test_ledger test_ledger.cpp)
xsr_add_test(xsr_test_coordinator test_coordinator.cpp)
xsr_add_test(xsr_test_adversarial test_adversarial.cpp)
xsr_add_test(xsr_test_property test_property.cpp)
xsr_add_test(xsr_test_concurrency test_concurrency.cpp)
xsr_add_test(xsr_test_process test_process.cpp)
xsr_add_test(xsr_test_cli test_cli.cpp)

# Remaining suites are appended as they are written.

target_compile_definitions(xsr_test_support PUBLIC XSR_TEST_BIN_DIR="$<TARGET_FILE_DIR:xsr_coordinator>")

# The package validation test installs the project and builds an independent
# consumer against the installed tree only.
if(XSR_BUILD_APPS)
  add_test(NAME xsr_test_package
           COMMAND ${CMAKE_COMMAND}
                   -DXSR_SOURCE_DIR=${CMAKE_SOURCE_DIR}
                   -DXSR_BINARY_DIR=${CMAKE_BINARY_DIR}
                   -DXSR_CXX_COMPILER=${CMAKE_CXX_COMPILER}
                   -DXSR_GENERATOR=${CMAKE_GENERATOR}
                   "-DXSR_BUILD_TYPE=$<CONFIG>"
                   -DXSR_VALIDATION_ROOT=${CMAKE_BINARY_DIR}/package-validation
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/package/validate_package.cmake)
  set_tests_properties(xsr_test_package PROPERTIES LABELS "xsr;package")
endif()
