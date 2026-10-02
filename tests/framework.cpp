// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#include "framework.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace xsrtest {
namespace {

std::vector<std::string> g_notes;

}  // namespace

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

void note(const std::string& message) { g_notes.push_back(message); }

int run_all(int argc, char** argv) {
  std::string filter;
  bool list_only = false;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index] == nullptr ? std::string() : std::string(argv[index]);
    if (argument.rfind("--filter=", 0) == 0) {
      filter = argument.substr(9);
    } else if (argument == "--list") {
      list_only = true;
    }
  }
  std::vector<TestCase>& tests = registry();
  std::sort(tests.begin(), tests.end(), [](const TestCase& a, const TestCase& b) {
    if (a.suite != b.suite) return a.suite < b.suite;
    return a.name < b.name;
  });
  if (list_only) {
    for (const TestCase& test : tests) {
      std::printf("%s.%s\n", test.suite.c_str(), test.name.c_str());
    }
    return 0;
  }
  std::size_t passed = 0;
  std::size_t failed = 0;
  std::size_t skipped = 0;
  const auto started = std::chrono::steady_clock::now();
  for (const TestCase& test : tests) {
    const std::string full = test.suite + "." + test.name;
    if (!filter.empty() && full.find(filter) == std::string::npos) {
      ++skipped;
      continue;
    }
    g_notes.clear();
    const auto test_started = std::chrono::steady_clock::now();
    bool ok = true;
    std::string failure;
    try {
      test.body();
    } catch (const TestFailure& error) {
      ok = false;
      failure = error.what();
    } catch (const std::exception& error) {
      ok = false;
      failure = std::string("unexpected exception: ") + error.what();
    } catch (...) {
      ok = false;
      failure = "unexpected non-standard exception";
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - test_started)
                             .count();
    if (ok) {
      ++passed;
      std::printf("[PASS] %-46s %6lld ms\n", full.c_str(), static_cast<long long>(elapsed));
    } else {
      ++failed;
      std::printf("[FAIL] %-46s %6lld ms\n       %s\n", full.c_str(),
                  static_cast<long long>(elapsed), failure.c_str());
      for (const std::string& single : g_notes) {
        std::printf("       note: %s\n", single.c_str());
      }
    }
    std::fflush(stdout);
  }
  const auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();
  std::printf("summary tests=%zu passed=%zu failed=%zu skipped=%zu elapsed_ms=%lld\n",
              passed + failed, passed, failed, skipped, static_cast<long long>(total_ms));
  std::fflush(stdout);
  return failed == 0 ? 0 : 1;
}

}  // namespace xsrtest

int main(int argc, char** argv) { return ::xsrtest::run_all(argc, argv); }
