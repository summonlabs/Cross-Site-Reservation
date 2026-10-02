// Cross-Site Reservation runtime — durable file primitives.
// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "xsr/error.hpp"

namespace xsr::platform {

/// Writes \p bytes to a temporary sibling file, flushes it to stable storage,
/// then atomically replaces \p path. When \p sync_directory is true the
/// containing directory is synced afterwards so the rename itself is durable.
Status write_file_atomic(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                         bool sync_directory);

/// Atomically replaces \p destination with \p source (both must be on the same volume).
Status atomic_replace(const std::filesystem::path& source, const std::filesystem::path& destination);

/// Flushes directory metadata (no-op on platforms without directory fsync).
Status sync_directory(const std::filesystem::path& directory);

/// Reads a whole file, refusing anything larger than \p max_bytes before allocating.
Result<std::vector<std::uint8_t>> read_file_bounded(const std::filesystem::path& path,
                                                    std::uint64_t max_bytes);

/// Current size of a file, or 0 when it does not exist.
Result<std::uint64_t> file_size_or_zero(const std::filesystem::path& path);

/// Removes a file if present; absent files are not an error.
Status remove_file_if_present(const std::filesystem::path& path);

/// Creates the directory and its parents.
Status ensure_directory(const std::filesystem::path& directory);

/// Monotonic-ish unique suffix for temporary file names.
std::string unique_suffix();

/// Opens a file with the platform's preferred stream API: fopen_s on MSVC,
/// fopen elsewhere. Returns nullptr on failure. This exists so that first-party
/// code can build with warnings-as-errors without disabling CRT deprecation
/// diagnostics globally.
std::FILE* open_file(const char* path, const char* mode) noexcept;

}  // namespace xsr::platform
