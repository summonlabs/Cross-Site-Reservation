// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
// Minimal first-party test harness: no third-party runtime dependency.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "xsr/error.hpp"

namespace xsrtest {

/// Thrown by a failed expectation and caught by the runner.
class TestFailure : public std::exception {
 public:
  explicit TestFailure(std::string message) : message_(std::move(message)) {}
  const char* what() const noexcept override { return message_.c_str(); }

 private:
  std::string message_;
};

struct TestCase {
  std::string suite;
  std::string name;
  std::function<void()> body;
};

std::vector<TestCase>& registry();
int run_all(int argc, char** argv);

struct Registrar {
  Registrar(const char* suite, const char* name, void (*fn)()) {
    registry().push_back(TestCase{suite, name, fn});
  }
};

[[noreturn]] inline void fail_at(const char* file, int line, const std::string& message) {
  throw TestFailure(std::string(file) + ":" + std::to_string(line) + ": " + message);
}

/// Records a diagnostic line that is printed when the test fails.
void note(const std::string& message);

#define XSR_TEST(suite, name)                                                            \
  static void xsr_test_body_##suite##_##name();                                          \
  static const ::xsrtest::Registrar xsr_test_reg_##suite##_##name(#suite, #name,          \
                                                                  &xsr_test_body_##suite##_##name); \
  static void xsr_test_body_##suite##_##name()

#define XSR_CHECK(condition)                                                             \
  do {                                                                                   \
    if (!(condition)) {                                                                  \
      ::xsrtest::fail_at(__FILE__, __LINE__, "expected: " #condition);                    \
    }                                                                                    \
  } while (false)

#define XSR_CHECK_MSG(condition, message)                                                \
  do {                                                                                   \
    if (!(condition)) {                                                                  \
      ::xsrtest::fail_at(__FILE__, __LINE__, std::string("expected: " #condition " - ") + (message)); \
    }                                                                                    \
  } while (false)

#define XSR_CHECK_EQ(actual, expected)                                                   \
  do {                                                                                   \
    const auto& xsr_actual_ = (actual);                                                  \
    const auto& xsr_expected_ = (expected);                                              \
    if (!(xsr_actual_ == xsr_expected_)) {                                               \
      ::xsrtest::fail_at(__FILE__, __LINE__,                                             \
                         std::string("expected " #actual " == " #expected));             \
    }                                                                                    \
  } while (false)

#define XSR_CHECK_OK(expression)                                                         \
  do {                                                                                   \
    auto&& xsr_status_ = (expression);                                                   \
    if (!xsr_status_.ok()) {                                                             \
      ::xsrtest::fail_at(__FILE__, __LINE__,                                             \
                         std::string("expected success from " #expression ": ") +        \
                             xsr_status_.error().to_string());                           \
    }                                                                                    \
  } while (false)

#define XSR_CHECK_ERR(expression, expected_code)                                         \
  do {                                                                                   \
    auto&& xsr_status_ = (expression);                                                   \
    if (xsr_status_.ok()) {                                                              \
      ::xsrtest::fail_at(__FILE__, __LINE__, std::string("expected failure from " #expression)); \
    }                                                                                    \
    if (xsr_status_.error().code != (expected_code)) {                                   \
      ::xsrtest::fail_at(__FILE__, __LINE__,                                             \
                         std::string("expected code ") + ::xsr::to_string(expected_code) + \
                             " from " #expression " but found " +                        \
                             ::xsr::to_string(xsr_status_.error().code) + ": " +         \
                             xsr_status_.error().to_string());                           \
    }                                                                                    \
  } while (false)

/// Extracts the value of an expected-success result, failing the test otherwise.
#define XSR_TAKE(expression)                                                             \
  ([&]() {                                                                               \
    auto&& xsr_result_ = (expression);                                                   \
    if (!xsr_result_.ok()) {                                                             \
      ::xsrtest::fail_at(__FILE__, __LINE__,                                             \
                         std::string("expected success from " #expression ": ") +        \
                             xsr_result_.error().to_string());                           \
    }                                                                                    \
    return std::move(xsr_result_).value();                                               \
  }())

}  // namespace xsrtest
