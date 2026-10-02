// Cross-Site Reservation runtime — durable append-only record store.
// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xsr/error.hpp"
#include "xsr/limits.hpp"

namespace xsr {

/// Every persisted record kind. Values are part of the on-disk format and must
/// never be renumbered.
enum class RecordKind : std::uint32_t {
  none = 0,
  coordinator_config = 1,
  participant_registration = 2,
  reservation_created = 3,
  reservation_prepared = 4,
  reservation_authorized = 5,
  reservation_committing = 6,
  reservation_active = 7,
  reservation_aborted = 8,
  reservation_released = 9,
  reservation_transferred = 10,
  reservation_revoked = 11,
  reservation_expired = 12,
  reservation_indeterminate = 13,
  reservation_transferred_out = 14,
  site_config = 40,
  site_prepare = 41,
  site_authorize = 42,
  site_commit = 43,
  site_abort = 44,
  site_release = 45,
  site_transfer = 46,
  site_revoke = 47,
  site_expire = 48,
};

const char* to_string(RecordKind kind) noexcept;
bool parse_record_kind(std::string_view text, RecordKind& out) noexcept;

struct Record {
  std::uint64_t sequence = 0;
  RecordKind kind = RecordKind::none;
  std::string payload;
};

/// Outcome of opening and replaying a store. Reported, never silently discarded.
struct ReplayReport {
  std::uint64_t last_sequence = 0;
  std::uint64_t snapshot_sequence = 0;
  bool has_snapshot = false;
  std::size_t records_applied = 0;
  std::size_t records_skipped_by_snapshot = 0;
  /// Bytes of an incomplete trailing record that were removed. Non-zero means a
  /// crash interrupted an append; the removed bytes were never committed.
  std::uint64_t torn_tail_bytes_truncated = 0;
  std::uint64_t log_bytes = 0;
  std::uint64_t snapshot_bytes = 0;
};

/// On-disk framing, exposed so that verification tooling and adversarial tests
/// can construct exact byte images.
namespace store_format {

inline constexpr std::size_t log_header_size = 32;
inline constexpr std::size_t record_header_size = 24;
inline constexpr std::size_t snapshot_header_size = 32;
inline constexpr std::uint32_t log_magic = 0x4C525358u;       // "XSRL" little-endian
inline constexpr std::uint32_t snapshot_magic = 0x53525358u;  // "XSRS" little-endian

void put_u32(std::span<std::uint8_t> out, std::size_t offset, std::uint32_t value) noexcept;
void put_u64(std::span<std::uint8_t> out, std::size_t offset, std::uint64_t value) noexcept;
std::uint32_t get_u32(std::span<const std::uint8_t> in, std::size_t offset) noexcept;
std::uint64_t get_u64(std::span<const std::uint8_t> in, std::size_t offset) noexcept;

std::vector<std::uint8_t> make_log_header();
Status validate_log_header(std::span<const std::uint8_t> header);

std::vector<std::uint8_t> make_record_header(std::uint32_t payload_length, RecordKind kind,
                                             std::uint64_t sequence);
Status validate_record_header(std::span<const std::uint8_t> header, std::size_t payload_available,
                              std::uint32_t& payload_length, RecordKind& kind, std::uint64_t& sequence,
                              std::uint32_t& payload_crc);

std::vector<std::uint8_t> make_snapshot_header(std::uint64_t sequence, std::uint32_t payload_length);
Status validate_snapshot_header(std::span<const std::uint8_t> header, std::uint64_t& sequence,
                               std::uint32_t& payload_length);

}  // namespace store_format

/// Single-writer durable log with snapshot compaction.
///
/// Durability contract: append() only buffers into the file. append()+commit()
/// is the durable boundary, where commit() flushes the stream and forces the
/// data to stable storage before returning. A crash between them may leave a
/// torn trailing record, which open() detects and removes.
///
/// Integrity contract: a torn trailing record is recoverable and reported.
/// Corruption anywhere else refuses to open the store; the runtime never
/// truncates through interior corruption and never claims a successful recovery
/// it cannot prove.
class RecordStore {
 public:
  RecordStore() = default;
  ~RecordStore();
  RecordStore(const RecordStore&) = delete;
  RecordStore& operator=(const RecordStore&) = delete;
  RecordStore(RecordStore&& other) noexcept;
  RecordStore& operator=(RecordStore&& other) noexcept;

  /// Opens (creating when absent) the store whose files are
  /// \p directory/\p name + ".xsrlog" and ".xsrsnap".
  static Result<RecordStore> open(const std::filesystem::path& directory, std::string_view name);

  /// Replays every committed record after the snapshot, in sequence order.
  Result<ReplayReport> replay(const std::function<Status(const Record&)>& apply);

  /// Buffers one record. Not durable until commit().
  Status append(RecordKind kind, std::string_view payload);
  /// Buffers one record and makes it durable before returning.
  Status append_commit(RecordKind kind, std::string_view payload);
  /// Flushes buffered records to stable storage.
  Status commit();

  /// Folds the whole committed log into \p snapshot_payload and restarts the log.
  ///
  /// The snapshot becomes authoritative for every sequence at or below the
  /// store's last sequence, and the log is rewritten to contain no records. A
  /// crash at any point leaves a readable store: if the snapshot lands but the
  /// log rewrite does not, replay simply skips the already-covered records.
  Status compact(std::string_view snapshot_payload);

  Result<std::vector<std::uint8_t>> load_snapshot() const;

  std::uint64_t last_sequence() const noexcept;
  std::uint64_t next_sequence() const noexcept;
  std::uint64_t log_bytes() const noexcept;
  std::uint64_t snapshot_sequence() const noexcept;
  bool has_snapshot() const noexcept;
  bool is_open() const noexcept;
  std::filesystem::path log_path() const;
  std::filesystem::path snapshot_path() const;

  Status close();

 private:
  Status flush_locked();
  Status open_log_locked(bool create);

  mutable std::mutex mutex_;
  std::filesystem::path log_path_;
  std::filesystem::path snapshot_path_;
  std::FILE* log_ = nullptr;
  std::uint64_t last_sequence_ = 0;
  std::uint64_t next_sequence_ = 1;
  std::uint64_t log_bytes_ = 0;
  std::uint64_t snapshot_sequence_ = 0;
  std::uint64_t snapshot_bytes_ = 0;
  std::uint64_t torn_tail_bytes_at_open_ = 0;
  bool has_snapshot_ = false;
  bool dirty_ = false;
};

/// Reads and verifies a store image without modifying it. Used by the CLI and by
/// tests that attack malformed state; it never truncates.
Result<ReplayReport> inspect_store(const std::filesystem::path& directory, std::string_view name,
                                   const std::function<Status(const Record&)>& apply);

}  // namespace xsr
