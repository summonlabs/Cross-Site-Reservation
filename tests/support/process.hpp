// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
// Real OS process control used by multiprocess validation.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "xsr/error.hpp"

namespace xsrtest {

/// A child process started by a test. Termination is explicit: no test relies on
/// a timeout, and every started process is either waited for or killed and
/// reaped before the test ends.
class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;

  static xsr::Result<ChildProcess> spawn(const std::filesystem::path& executable,
                                    const std::vector<std::string>& arguments,
                                    const std::filesystem::path& stdout_path,
                                    const std::filesystem::path& stderr_path);

  bool valid() const noexcept;
  bool running();
  /// Blocks until the process terminates and returns its exit code.
  int wait();
  /// Terminates immediately and reaps. Safe to call more than once.
  void kill();
  std::uint64_t id() const noexcept { return id_; }
  /// Index of this child in the case's spawn order, for diagnostics.
  std::filesystem::path stdout_path() const { return stdout_path_; }
  std::filesystem::path stderr_path() const { return stderr_path_; }

 private:
  void reap();
  void close_handles();

#if defined(_WIN32)
  void* process_ = nullptr;
#else
  int pid_ = -1;
#endif
  std::uint64_t id_ = 0;
  std::filesystem::path stdout_path_;
  std::filesystem::path stderr_path_;
  int exit_code_ = -1;
  bool reaped_ = true;
};

/// Reads a text file, returning an empty string when it is absent.
std::string read_text_file(const std::filesystem::path& path);
/// Waits (bounded) for a file to appear. Returns false when the budget is spent.
bool wait_for_file(const std::filesystem::path& path, int attempts, int interval_ms);
/// Removes a directory tree if present.
void remove_tree(const std::filesystem::path& path);

}  // namespace xsrtest
