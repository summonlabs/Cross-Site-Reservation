// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#include "process.hpp"

#include "xsr/platform.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace xsrtest {

// Test support code speaks the boundary vocabulary directly.
using namespace xsr;

namespace {

#if defined(_WIN32)
std::string quote_argument(const std::string& argument) {
  std::string out = "\"";
  for (char c : argument) {
    if (c == '"') {
      out += "\\\"";
    } else {
      out.push_back(c);
    }
  }
  out.push_back('"');
  return out;
}

std::wstring widen(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(size), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size);
  return out;
}
#endif

}  // namespace

std::string read_text_file(const std::filesystem::path& path) {
  std::FILE* file = platform::open_file(path.string().c_str(), "rb");
  if (file == nullptr) return {};
  std::string out;
  char buffer[4096];
  std::size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    out.append(buffer, got);
  }
  std::fclose(file);
  return out;
}

bool wait_for_file(const std::filesystem::path& path, int attempts, int interval_ms) {
  for (int attempt = 0; attempt < attempts; ++attempt) {
    if (std::filesystem::exists(path)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
  }
  return std::filesystem::exists(path);
}

void remove_tree(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::remove_all(path, error);
}

ChildProcess::~ChildProcess() { kill(); }

ChildProcess::ChildProcess(ChildProcess&& other) noexcept {
#if defined(_WIN32)
  process_ = other.process_;
  other.process_ = nullptr;
#else
  pid_ = other.pid_;
  other.pid_ = -1;
#endif
  id_ = other.id_;
  stdout_path_ = std::move(other.stdout_path_);
  stderr_path_ = std::move(other.stderr_path_);
  exit_code_ = other.exit_code_;
  reaped_ = other.reaped_;
  other.reaped_ = true;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this == &other) return *this;
  kill();
#if defined(_WIN32)
  process_ = other.process_;
  other.process_ = nullptr;
#else
  pid_ = other.pid_;
  other.pid_ = -1;
#endif
  id_ = other.id_;
  stdout_path_ = std::move(other.stdout_path_);
  stderr_path_ = std::move(other.stderr_path_);
  exit_code_ = other.exit_code_;
  reaped_ = other.reaped_;
  other.reaped_ = true;
  return *this;
}

bool ChildProcess::valid() const noexcept {
#if defined(_WIN32)
  return process_ != nullptr;
#else
  return pid_ > 0;
#endif
}

xsr::Result<ChildProcess> ChildProcess::spawn(const std::filesystem::path& executable,
                                              const std::vector<std::string>& arguments,
                                              const std::filesystem::path& stdout_path,
                                              const std::filesystem::path& stderr_path) {
  if (!std::filesystem::exists(executable)) {
    return make_error(ErrorCode::not_found, "test binary does not exist", executable.string());
  }
  ChildProcess child;
  child.stdout_path_ = stdout_path;
  child.stderr_path_ = stderr_path;
#if defined(_WIN32)
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  const std::wstring out_w = widen(stdout_path.string());
  const std::wstring err_w = widen(stderr_path.string());
  HANDLE out = ::CreateFileW(out_w.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (out == INVALID_HANDLE_VALUE) {
    return make_error(ErrorCode::io_error, "could not create the child stdout file", stdout_path.string());
  }
  HANDLE err = ::CreateFileW(err_w.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (err == INVALID_HANDLE_VALUE) {
    ::CloseHandle(out);
    return make_error(ErrorCode::io_error, "could not create the child stderr file", stderr_path.string());
  }
  HANDLE nul = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = out;
  startup.hStdError = err;
  startup.hStdInput = nul == INVALID_HANDLE_VALUE ? nullptr : nul;
  PROCESS_INFORMATION info{};

  std::string command = quote_argument(executable.string());
  for (const std::string& argument : arguments) {
    command.push_back(' ');
    command += quote_argument(argument);
  }
  std::wstring command_w = widen(command);
  const std::wstring directory_w = widen(executable.parent_path().string());
  const BOOL created =
      ::CreateProcessW(nullptr, command_w.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                       directory_w.empty() ? nullptr : directory_w.c_str(), &startup, &info);
  ::CloseHandle(out);
  ::CloseHandle(err);
  if (nul != INVALID_HANDLE_VALUE) ::CloseHandle(nul);
  if (created == 0) {
    return make_error(ErrorCode::io_error, "CreateProcess failed for the test binary",
                      executable.string());
  }
  ::CloseHandle(info.hThread);
  child.process_ = info.hProcess;
  child.id_ = static_cast<std::uint64_t>(info.dwProcessId);
  child.reaped_ = false;
  return child;
#else
  const pid_t pid = ::fork();
  if (pid < 0) {
    return make_error(ErrorCode::io_error, "fork failed");
  }
  if (pid == 0) {
    const int out = ::open(stdout_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    const int err = ::open(stderr_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    const int in = ::open("/dev/null", O_RDONLY);
    if (in >= 0) ::dup2(in, STDIN_FILENO);
    if (out >= 0) ::dup2(out, STDOUT_FILENO);
    if (err >= 0) ::dup2(err, STDERR_FILENO);
    std::vector<std::string> storage;
    storage.reserve(arguments.size() + 1);
    storage.push_back(executable.string());
    for (const std::string& argument : arguments) storage.push_back(argument);
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (std::string& entry : storage) argv.push_back(entry.data());
    argv.push_back(nullptr);
    ::execv(executable.c_str(), argv.data());
    ::_exit(127);
  }
  child.pid_ = static_cast<int>(pid);
  child.id_ = static_cast<std::uint64_t>(pid);
  child.reaped_ = false;
  return child;
#endif
}

bool ChildProcess::running() {
  if (!valid() || reaped_) return false;
#if defined(_WIN32)
  return ::WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_TIMEOUT;
#else
  int status = 0;
  const pid_t result = ::waitpid(pid_, &status, WNOHANG);
  if (result == 0) return true;
  reaped_ = true;
  exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return false;
#endif
}

void ChildProcess::reap() {
  if (reaped_ || !valid()) return;
#if defined(_WIN32)
  ::WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
  DWORD code = 0;
  ::GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
  exit_code_ = static_cast<int>(code);
#else
  int status = 0;
  ::waitpid(pid_, &status, 0);
  exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
  reaped_ = true;
}

int ChildProcess::wait() {
  reap();
  close_handles();
  return exit_code_;
}

void ChildProcess::kill() {
  if (!valid()) return;
  if (!reaped_) {
#if defined(_WIN32)
    ::TerminateProcess(static_cast<HANDLE>(process_), 9);
#else
    ::kill(pid_, SIGKILL);
#endif
    reap();
  }
  close_handles();
}

void ChildProcess::close_handles() {
#if defined(_WIN32)
  if (process_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(process_));
    process_ = nullptr;
  }
#else
  pid_ = -1;
#endif
}

}  // namespace xsrtest
