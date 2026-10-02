// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#include "xsr/platform.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <io.h>

#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace xsr::platform {
namespace {

Status flush_file_to_storage(std::FILE* file, const std::filesystem::path& path) {
  if (std::fflush(file) != 0) {
    return make_error(ErrorCode::io_error, "fflush failed", path.string());
  }
#if defined(_WIN32)
  if (::_commit(::_fileno(file)) != 0) {
    return make_error(ErrorCode::io_error, "_commit failed", path.string());
  }
#else
  if (::fsync(::fileno(file)) != 0) {
    return make_error(ErrorCode::io_error, "fsync failed", path.string());
  }
#endif
  return ok_status();
}

}  // namespace

Status atomic_replace(const std::filesystem::path& source, const std::filesystem::path& destination) {
#if defined(_WIN32)
  if (::MoveFileExW(source.c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    return make_error(ErrorCode::io_error, "MoveFileEx failed", destination.string());
  }
  return ok_status();
#else
  if (::rename(source.c_str(), destination.c_str()) != 0) {
    return make_error(ErrorCode::io_error, std::string("rename failed: ") + std::strerror(errno),
                      destination.string());
  }
  return ok_status();
#endif
}

Status sync_directory(const std::filesystem::path& directory) {
#if defined(_WIN32)
  (void)directory;
  return ok_status();
#else
  const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) {
    return make_error(ErrorCode::io_error, "opening the directory for fsync failed", directory.string());
  }
  const int rc = ::fsync(fd);
  ::close(fd);
  if (rc != 0) {
    return make_error(ErrorCode::io_error, "directory fsync failed", directory.string());
  }
  return ok_status();
#endif
}

std::FILE* open_file(const char* path, const char* mode) noexcept {
#if defined(_MSC_VER)
  // _fsopen is the non-deprecated MSVC entry point and makes the sharing mode
  // explicit: the runtime reads and writes its own journal files while they are
  // open, and other processes may inspect them concurrently.
  return ::_fsopen(path, mode, _SH_DENYNO);
#else
  return std::fopen(path, mode);
#endif
}

Status write_file_atomic(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                         bool sync_directory_after) {
  auto parent = path.parent_path();
  if (!parent.empty()) {
    XSR_RETURN_IF_ERROR(ensure_directory(parent));
  }
  std::filesystem::path temporary = path;
  temporary += ".tmp";
  std::FILE* file = open_file(temporary.string().c_str(), "wb");
  if (file == nullptr) {
    return make_error(ErrorCode::io_error, "creating a temporary file failed", temporary.string());
  }
  if (!bytes.empty()) {
    const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file);
    if (written != bytes.size()) {
      std::fclose(file);
      std::filesystem::remove(temporary);
      return make_error(ErrorCode::io_error, "writing a temporary file failed", temporary.string());
    }
  }
  Status flushed = flush_file_to_storage(file, temporary);
  std::fclose(file);
  if (!flushed.ok()) {
    std::filesystem::remove(temporary);
    return flushed.error();
  }
  Status replaced = atomic_replace(temporary, path);
  if (!replaced.ok()) {
    std::filesystem::remove(temporary);
    return replaced.error();
  }
  if (sync_directory_after && !parent.empty()) {
    return sync_directory(parent);
  }
  return ok_status();
}

Result<std::vector<std::uint8_t>> read_file_bounded(const std::filesystem::path& path, std::uint64_t max_bytes) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    return make_error(ErrorCode::not_found, "file is not readable", path.string());
  }
  if (size > max_bytes) {
    return make_error(ErrorCode::too_large, "file exceeds the allowed byte budget", path.string());
  }
  std::FILE* file = open_file(path.string().c_str(), "rb");
  if (file == nullptr) {
    return make_error(ErrorCode::not_found, "file could not be opened", path.string());
  }
  std::vector<std::uint8_t> out(static_cast<std::size_t>(size));
  std::size_t offset = 0;
  while (offset < out.size()) {
    const std::size_t got = std::fread(out.data() + offset, 1, out.size() - offset, file);
    if (got == 0) {
      std::fclose(file);
      return make_error(ErrorCode::io_error, "file shrank while being read", path.string());
    }
    offset += got;
  }
  std::fclose(file);
  return out;
}

Result<std::uint64_t> file_size_or_zero(const std::filesystem::path& path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) return std::uint64_t{0};
  return static_cast<std::uint64_t>(size);
}

Status remove_file_if_present(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
  if (ec) {
    return make_error(ErrorCode::io_error, "removing a file failed: " + ec.message(), path.string());
  }
  return ok_status();
}

Status ensure_directory(const std::filesystem::path& directory) {
  std::error_code ec;
  if (std::filesystem::exists(directory, ec)) {
    if (!std::filesystem::is_directory(directory, ec)) {
      return make_error(ErrorCode::invalid_argument, "path exists but is not a directory", directory.string());
    }
    return ok_status();
  }
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    return make_error(ErrorCode::io_error, "creating a directory failed: " + ec.message(), directory.string());
  }
  return ok_status();
}

std::string unique_suffix() {
  static std::atomic<std::uint64_t> counter{0};
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto tick = counter.fetch_add(1, std::memory_order_relaxed);
  return std::to_string(static_cast<unsigned long long>(now)) + "-" + std::to_string(tick);
}

}  // namespace xsr::platform
