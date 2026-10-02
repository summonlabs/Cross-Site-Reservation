// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#include "xsr/store.hpp"

#include <algorithm>
#include <cstring>

#include "xsr/digest.hpp"
#include "xsr/platform.hpp"
#include "xsr/version.hpp"

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace xsr {
namespace {

/// Bounded scan outcomes. A scan either reaches the end of the log, finds an
/// incomplete trailing record (torn tail), or refuses the image as corrupt.
struct ScanOutcome {
  std::uint64_t last_sequence = 0;
  std::uint64_t good_bytes = 0;
  std::uint64_t torn_tail_bytes = 0;
  std::size_t applied = 0;
  std::size_t skipped = 0;
  bool had_torn_tail = false;
};

std::int64_t stream_tell(std::FILE* file) {
#if defined(_WIN32)
  return static_cast<std::int64_t>(::_ftelli64(file));
#else
  return static_cast<std::int64_t>(::ftello(file));
#endif
}

int stream_seek(std::FILE* file, std::int64_t offset) {
#if defined(_WIN32)
  return ::_fseeki64(file, offset, SEEK_SET);
#else
  return ::fseeko(file, static_cast<off_t>(offset), SEEK_SET);
#endif
}

Status stream_truncate(std::FILE* file, std::uint64_t size) {
  if (std::fflush(file) != 0) {
    return make_error(ErrorCode::io_error, "flush before truncation failed");
  }
#if defined(_WIN32)
  if (::_chsize_s(::_fileno(file), static_cast<__int64>(size)) != 0) {
    return make_error(ErrorCode::io_error, "_chsize_s failed");
  }
  if (::_commit(::_fileno(file)) != 0) {
    return make_error(ErrorCode::io_error, "_commit after truncation failed");
  }
#else
  if (::ftruncate(::fileno(file), static_cast<off_t>(size)) != 0) {
    return make_error(ErrorCode::io_error, "ftruncate failed");
  }
  if (::fsync(::fileno(file)) != 0) {
    return make_error(ErrorCode::io_error, "fsync after truncation failed");
  }
#endif
  return ok_status();
}

Status stream_sync(std::FILE* file) {
  if (std::fflush(file) != 0) {
    return make_error(ErrorCode::io_error, "fflush failed");
  }
#if defined(_WIN32)
  if (::_commit(::_fileno(file)) != 0) {
    return make_error(ErrorCode::io_error, "_commit failed");
  }
#else
  if (::fsync(::fileno(file)) != 0) {
    return make_error(ErrorCode::io_error, "fsync failed");
  }
#endif
  return ok_status();
}

/// Seeks to the end of the stream.
int stream_seek_end(std::FILE* file) {
#if defined(_WIN32)
  return ::_fseeki64(file, 0, SEEK_END);
#else
  return ::fseeko(file, 0, SEEK_END);
#endif
}

Result<std::uint64_t> stream_size(std::FILE* file) {
  const std::int64_t position = stream_tell(file);
  if (position < 0) return make_error(ErrorCode::io_error, "ftell failed");
  if (stream_seek_end(file) != 0) return make_error(ErrorCode::io_error, "seek to end failed");
  const std::int64_t end = stream_tell(file);
  if (end < 0) return make_error(ErrorCode::io_error, "ftell at end failed");
  if (stream_seek(file, position) != 0) return make_error(ErrorCode::io_error, "seek restore failed");
  return static_cast<std::uint64_t>(end);
}

bool read_exact(std::FILE* file, std::span<std::uint8_t> out, std::size_t& got) {
  got = 0;
  while (got < out.size()) {
    const std::size_t chunk = std::fread(out.data() + got, 1, out.size() - got, file);
    if (chunk == 0) return false;
    got += chunk;
  }
  return true;
}

/// True when every byte from \p offset to end of file is zero. Used to tell a
/// preallocated zero tail apart from genuine interior corruption.
bool region_is_zero(std::FILE* file, std::uint64_t offset, std::uint64_t end) {
  if (stream_seek(file, static_cast<std::int64_t>(offset)) != 0) return false;
  std::uint8_t buffer[64 * 1024];
  std::uint64_t position = offset;
  while (position < end) {
    const std::uint64_t remaining = end - position;
    const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, sizeof(buffer)));
    std::size_t got = 0;
    if (!read_exact(file, std::span<std::uint8_t>(buffer, want), got) || got != want) return false;
    for (std::size_t i = 0; i < got; ++i) {
      if (buffer[i] != 0) return false;
    }
    position += got;
  }
  return true;
}

std::filesystem::path log_path_for(const std::filesystem::path& directory, std::string_view name) {
  return directory / (std::string(name) + ".xsrlog");
}

std::filesystem::path snapshot_path_for(const std::filesystem::path& directory, std::string_view name) {
  return directory / (std::string(name) + ".xsrsnap");
}

}  // namespace

namespace store_format {

void put_u32(std::span<std::uint8_t> out, std::size_t offset, std::uint32_t value) noexcept {
  out[offset + 0] = static_cast<std::uint8_t>(value & 0xFFu);
  out[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
  out[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
  out[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
}

void put_u64(std::span<std::uint8_t> out, std::size_t offset, std::uint64_t value) noexcept {
  put_u32(out, offset, static_cast<std::uint32_t>(value & 0xFFFFFFFFu));
  put_u32(out, offset + 4, static_cast<std::uint32_t>((value >> 32) & 0xFFFFFFFFu));
}

std::uint32_t get_u32(std::span<const std::uint8_t> in, std::size_t offset) noexcept {
  return static_cast<std::uint32_t>(in[offset]) | (static_cast<std::uint32_t>(in[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(in[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(in[offset + 3]) << 24);
}

std::uint64_t get_u64(std::span<const std::uint8_t> in, std::size_t offset) noexcept {
  return static_cast<std::uint64_t>(get_u32(in, offset)) |
         (static_cast<std::uint64_t>(get_u32(in, offset + 4)) << 32);
}

std::vector<std::uint8_t> make_log_header() {
  std::vector<std::uint8_t> header(log_header_size, 0);
  std::span<std::uint8_t> span(header);
  put_u32(span, 0, log_magic);
  span[4] = static_cast<std::uint8_t>(store_format_version & 0xFFu);
  span[5] = static_cast<std::uint8_t>((store_format_version >> 8) & 0xFFu);
  span[6] = static_cast<std::uint8_t>(log_header_size & 0xFFu);
  span[7] = static_cast<std::uint8_t>((log_header_size >> 8) & 0xFFu);
  put_u32(span, 8, canonical_version);
  put_u32(span, 12, crc32c(span.subspan(0, 12)));
  return header;
}

Status validate_log_header(std::span<const std::uint8_t> header) {
  if (header.size() < log_header_size) {
    return make_error(ErrorCode::corrupt, "log header is truncated");
  }
  if (get_u32(header, 0) != log_magic) {
    return make_error(ErrorCode::corrupt, "log header magic does not match");
  }
  const std::uint32_t version = static_cast<std::uint32_t>(header[4]) |
                                (static_cast<std::uint32_t>(header[5]) << 8);
  if (version != store_format_version) {
    return make_error(ErrorCode::version_mismatch, "log format version is not supported",
                      std::to_string(version));
  }
  const std::uint32_t header_size = static_cast<std::uint32_t>(header[6]) |
                                    (static_cast<std::uint32_t>(header[7]) << 8);
  if (header_size != log_header_size) {
    return make_error(ErrorCode::corrupt, "log header size is not the expected width");
  }
  if (get_u32(header, 8) != canonical_version) {
    return make_error(ErrorCode::version_mismatch, "canonical encoding version is not supported");
  }
  if (crc32c(header.subspan(0, 12)) != get_u32(header, 12)) {
    return make_error(ErrorCode::corrupt, "log header checksum failed");
  }
  for (std::size_t i = 16; i < log_header_size; ++i) {
    if (header[i] != 0) {
      return make_error(ErrorCode::corrupt, "reserved log header bytes are not zero");
    }
  }
  return ok_status();
}

std::vector<std::uint8_t> make_record_header(std::uint32_t payload_length, RecordKind kind,
                                             std::uint64_t sequence) {
  std::vector<std::uint8_t> header(record_header_size, 0);
  std::span<std::uint8_t> span(header);
  put_u32(span, 0, payload_length);
  put_u32(span, 4, static_cast<std::uint32_t>(kind));
  put_u64(span, 8, sequence);
  return header;
}

Status validate_record_header(std::span<const std::uint8_t> header, std::size_t payload_available,
                              std::uint32_t& payload_length, RecordKind& kind, std::uint64_t& sequence,
                              std::uint32_t& payload_crc) {
  if (header.size() < record_header_size) {
    return make_error(ErrorCode::corrupt, "record header is truncated");
  }
  if (crc32c(header.subspan(0, 20)) != get_u32(header, 20)) {
    return make_error(ErrorCode::corrupt, "record header checksum failed");
  }
  payload_length = get_u32(header, 0);
  kind = static_cast<RecordKind>(get_u32(header, 4));
  sequence = get_u64(header, 8);
  payload_crc = get_u32(header, 16);
  if (payload_length > limits::max_record_payload) {
    return make_error(ErrorCode::corrupt, "record payload length exceeds the allowed budget");
  }
  if (payload_available < payload_length) {
    return make_error(ErrorCode::torn_tail, "record payload is incomplete");
  }
  return ok_status();
}

std::vector<std::uint8_t> make_snapshot_header(std::uint64_t sequence, std::uint32_t payload_length) {
  std::vector<std::uint8_t> header(snapshot_header_size, 0);
  std::span<std::uint8_t> span(header);
  put_u32(span, 0, snapshot_magic);
  span[4] = static_cast<std::uint8_t>(snapshot_format_version & 0xFFu);
  span[5] = static_cast<std::uint8_t>((snapshot_format_version >> 8) & 0xFFu);
  span[6] = static_cast<std::uint8_t>(snapshot_header_size & 0xFFu);
  span[7] = static_cast<std::uint8_t>((snapshot_header_size >> 8) & 0xFFu);
  put_u64(span, 8, sequence);
  put_u32(span, 16, payload_length);
  return header;
}

Status validate_snapshot_header(std::span<const std::uint8_t> header, std::uint64_t& sequence,
                               std::uint32_t& payload_length) {
  if (header.size() < snapshot_header_size) {
    return make_error(ErrorCode::corrupt, "snapshot header is truncated");
  }
  if (get_u32(header, 0) != snapshot_magic) {
    return make_error(ErrorCode::corrupt, "snapshot header magic does not match");
  }
  const std::uint32_t version = static_cast<std::uint32_t>(header[4]) |
                                (static_cast<std::uint32_t>(header[5]) << 8);
  if (version != snapshot_format_version) {
    return make_error(ErrorCode::version_mismatch, "snapshot format version is not supported",
                      std::to_string(version));
  }
  const std::uint32_t header_size = static_cast<std::uint32_t>(header[6]) |
                                    (static_cast<std::uint32_t>(header[7]) << 8);
  if (header_size != snapshot_header_size) {
    return make_error(ErrorCode::corrupt, "snapshot header size is not the expected width");
  }
  if (crc32c(header.subspan(0, 24)) != get_u32(header, 24)) {
    return make_error(ErrorCode::corrupt, "snapshot header checksum failed");
  }
  sequence = get_u64(header, 8);
  payload_length = get_u32(header, 16);
  if (payload_length > limits::max_snapshot_payload) {
    return make_error(ErrorCode::corrupt, "snapshot payload exceeds the allowed budget");
  }
  return ok_status();
}

}  // namespace store_format

namespace {

bool is_known_record_kind(RecordKind kind) noexcept {
  switch (kind) {
    case RecordKind::none:
    case RecordKind::coordinator_config:
    case RecordKind::participant_registration:
    case RecordKind::reservation_created:
    case RecordKind::reservation_prepared:
    case RecordKind::reservation_authorized:
    case RecordKind::reservation_committing:
    case RecordKind::reservation_active:
    case RecordKind::reservation_aborted:
    case RecordKind::reservation_released:
    case RecordKind::reservation_transferred:
    case RecordKind::reservation_revoked:
    case RecordKind::reservation_expired:
    case RecordKind::reservation_indeterminate:
    case RecordKind::reservation_transferred_out:
    case RecordKind::site_config:
    case RecordKind::site_prepare:
    case RecordKind::site_authorize:
    case RecordKind::site_commit:
    case RecordKind::site_abort:
    case RecordKind::site_release:
    case RecordKind::site_transfer:
    case RecordKind::site_revoke:
    case RecordKind::site_expire:
      return true;
  }
  return false;
}

}  // namespace

const char* to_string(RecordKind kind) noexcept {
  switch (kind) {
    case RecordKind::none: return "none";
    case RecordKind::coordinator_config: return "coordinator-config";
    case RecordKind::participant_registration: return "participant-registration";
    case RecordKind::reservation_created: return "reservation-created";
    case RecordKind::reservation_prepared: return "reservation-prepared";
    case RecordKind::reservation_authorized: return "reservation-authorized";
    case RecordKind::reservation_committing: return "reservation-committing";
    case RecordKind::reservation_active: return "reservation-active";
    case RecordKind::reservation_aborted: return "reservation-aborted";
    case RecordKind::reservation_released: return "reservation-released";
    case RecordKind::reservation_transferred: return "reservation-transferred";
    case RecordKind::reservation_revoked: return "reservation-revoked";
    case RecordKind::reservation_expired: return "reservation-expired";
    case RecordKind::reservation_indeterminate: return "reservation-indeterminate";
    case RecordKind::reservation_transferred_out: return "reservation-transferred-out";
    case RecordKind::site_config: return "site-config";
    case RecordKind::site_prepare: return "site-prepare";
    case RecordKind::site_authorize: return "site-authorize";
    case RecordKind::site_commit: return "site-commit";
    case RecordKind::site_abort: return "site-abort";
    case RecordKind::site_release: return "site-release";
    case RecordKind::site_transfer: return "site-transfer";
    case RecordKind::site_revoke: return "site-revoke";
    case RecordKind::site_expire: return "site-expire";
  }
  return "unknown";
}

bool parse_record_kind(std::string_view text, RecordKind& out) noexcept {
  static constexpr RecordKind kAll[] = {
      RecordKind::none,         RecordKind::coordinator_config,
      RecordKind::participant_registration, RecordKind::reservation_created,
      RecordKind::reservation_prepared,     RecordKind::reservation_authorized,
      RecordKind::reservation_committing,   RecordKind::reservation_active,
      RecordKind::reservation_aborted,      RecordKind::reservation_released,
      RecordKind::reservation_transferred,  RecordKind::reservation_revoked,
      RecordKind::reservation_expired,      RecordKind::reservation_indeterminate,
      RecordKind::reservation_transferred_out, RecordKind::site_config,
      RecordKind::site_prepare,             RecordKind::site_authorize,
      RecordKind::site_commit,              RecordKind::site_abort,
      RecordKind::site_release,             RecordKind::site_transfer,
      RecordKind::site_revoke,              RecordKind::site_expire};
  for (RecordKind kind : kAll) {
    if (text == to_string(kind)) {
      out = kind;
      return true;
    }
  }
  return false;
}

namespace {

/// Streams one validated scan over the log.
///
/// Records at or below \p skip_through are already folded into the snapshot:
/// they are still integrity-checked but not applied. Sequence contiguity is
/// enforced relative to the first record present in the log, which is what a
/// crash between the snapshot write and the log rewrite leaves behind.
Status scan_log(std::FILE* file, std::uint64_t skip_through, const std::function<Status(const Record&)>* apply,
                bool allow_truncate, ScanOutcome& outcome) {
  auto size_result = stream_size(file);
  if (!size_result.ok()) return size_result.error();
  const std::uint64_t size = size_result.value();
  if (size < store_format::log_header_size) {
    return make_error(ErrorCode::corrupt, "log file is shorter than its header");
  }
  if (stream_seek(file, 0) != 0) {
    return make_error(ErrorCode::io_error, "seeking to the start of the log failed");
  }
  std::vector<std::uint8_t> header(store_format::log_header_size);
  std::size_t got = 0;
  if (!read_exact(file, header, got) || got != header.size()) {
    return make_error(ErrorCode::corrupt, "log header could not be read");
  }
  XSR_RETURN_IF_ERROR(store_format::validate_log_header(header));

  std::uint64_t offset = store_format::log_header_size;
  std::uint64_t expected = 0;
  bool first_record = true;
  std::uint64_t last = 0;
  std::vector<std::uint8_t> record_header(store_format::record_header_size);
  std::vector<std::uint8_t> payload;

  while (offset < size) {
    const std::uint64_t remaining = size - offset;
    if (remaining < store_format::record_header_size) {
      outcome.torn_tail_bytes = remaining;
      outcome.had_torn_tail = true;
      break;
    }
    if (stream_seek(file, static_cast<std::int64_t>(offset)) != 0) {
      return make_error(ErrorCode::io_error, "seeking within the log failed");
    }
    if (!read_exact(file, record_header, got) || got != record_header.size()) {
      outcome.torn_tail_bytes = remaining;
      outcome.had_torn_tail = true;
      break;
    }
    const std::uint32_t header_crc = store_format::get_u32(record_header, 20);
    if (crc32c(std::span<const std::uint8_t>(record_header).subspan(0, 20)) != header_crc) {
      if (region_is_zero(file, offset, size)) {
        outcome.torn_tail_bytes = remaining;
        outcome.had_torn_tail = true;
        break;
      }
      return make_error(ErrorCode::corrupt, "record header checksum failed",
                        "offset " + std::to_string(offset));
    }
    const std::uint32_t payload_length = store_format::get_u32(record_header, 0);
    const RecordKind kind = static_cast<RecordKind>(store_format::get_u32(record_header, 4));
    const std::uint64_t sequence = store_format::get_u64(record_header, 8);
    const std::uint32_t payload_crc = store_format::get_u32(record_header, 16);
    if (payload_length > limits::max_record_payload) {
      return make_error(ErrorCode::corrupt, "record payload length exceeds the allowed budget",
                        "offset " + std::to_string(offset));
    }
    if (!is_known_record_kind(kind)) {
      return make_error(ErrorCode::corrupt, "record kind is not recognised",
                        "offset " + std::to_string(offset));
    }
    if (remaining - store_format::record_header_size < payload_length) {
      outcome.torn_tail_bytes = remaining;
      outcome.had_torn_tail = true;
      break;
    }
    if (!first_record && sequence != expected) {
      return make_error(ErrorCode::corrupt, "record sequence is not contiguous",
                        "expected " + std::to_string(expected) + " but found " + std::to_string(sequence));
    }
    first_record = false;
    expected = sequence + 1;
    if (apply != nullptr) {
      if (stream_seek(file, static_cast<std::int64_t>(offset + store_format::record_header_size)) != 0) {
        return make_error(ErrorCode::io_error, "seeking to the record payload failed");
      }
      payload.assign(payload_length, 0);
      if (!read_exact(file, payload, got) || got != payload.size()) {
        outcome.torn_tail_bytes = remaining;
        outcome.had_torn_tail = true;
        break;
      }
      if (crc32c(payload) != payload_crc) {
        return make_error(ErrorCode::corrupt, "record payload checksum failed",
                          "offset " + std::to_string(offset));
      }
      if (sequence > skip_through) {
        Record record;
        record.sequence = sequence;
        record.kind = kind;
        record.payload = std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
        XSR_RETURN_IF_ERROR((*apply)(record));
        ++outcome.applied;
      } else {
        ++outcome.skipped;
      }
    } else {
      // Structure-only pass: still verify the payload checksum and width.
      std::vector<std::uint8_t> local(payload_length, 0);
      if (stream_seek(file, static_cast<std::int64_t>(offset + store_format::record_header_size)) != 0) {
        return make_error(ErrorCode::io_error, "seeking to the record payload failed");
      }
      if (!read_exact(file, local, got) || got != local.size()) {
        outcome.torn_tail_bytes = remaining;
        outcome.had_torn_tail = true;
        break;
      }
      if (crc32c(local) != payload_crc) {
        return make_error(ErrorCode::corrupt, "record payload checksum failed",
                          "offset " + std::to_string(offset));
      }
    }
    last = sequence;
    offset += store_format::record_header_size + payload_length;
    outcome.good_bytes = offset;
  }

  outcome.last_sequence = last;
  if (!outcome.had_torn_tail) {
    outcome.good_bytes = size;
    outcome.torn_tail_bytes = 0;
  } else if (allow_truncate) {
    XSR_RETURN_IF_ERROR(stream_truncate(file, outcome.good_bytes));
  }
  return ok_status();
}

}  // namespace

RecordStore::~RecordStore() {
  if (log_ != nullptr) {
    std::fclose(log_);
    log_ = nullptr;
  }
}

RecordStore::RecordStore(RecordStore&& other) noexcept {
  std::lock_guard<std::mutex> guard(other.mutex_);
  log_path_ = std::move(other.log_path_);
  snapshot_path_ = std::move(other.snapshot_path_);
  log_ = other.log_;
  last_sequence_ = other.last_sequence_;
  next_sequence_ = other.next_sequence_;
  log_bytes_ = other.log_bytes_;
  snapshot_sequence_ = other.snapshot_sequence_;
  snapshot_bytes_ = other.snapshot_bytes_;
  has_snapshot_ = other.has_snapshot_;
  torn_tail_bytes_at_open_ = other.torn_tail_bytes_at_open_;
  dirty_ = other.dirty_;
  other.log_ = nullptr;
}

RecordStore& RecordStore::operator=(RecordStore&& other) noexcept {
  if (this == &other) return *this;
  if (log_ != nullptr) {
    std::fclose(log_);
    log_ = nullptr;
  }
  std::scoped_lock guard(mutex_, other.mutex_);
  log_path_ = std::move(other.log_path_);
  snapshot_path_ = std::move(other.snapshot_path_);
  log_ = other.log_;
  last_sequence_ = other.last_sequence_;
  next_sequence_ = other.next_sequence_;
  log_bytes_ = other.log_bytes_;
  snapshot_sequence_ = other.snapshot_sequence_;
  snapshot_bytes_ = other.snapshot_bytes_;
  has_snapshot_ = other.has_snapshot_;
  torn_tail_bytes_at_open_ = other.torn_tail_bytes_at_open_;
  dirty_ = other.dirty_;
  other.log_ = nullptr;
  return *this;
}

Result<RecordStore> RecordStore::open(const std::filesystem::path& directory, std::string_view name) {
  if (name.empty() || name.size() > limits::max_name_length) {
    return make_error(ErrorCode::invalid_argument, "store name is empty or too long");
  }
  if (name == "." || name == "..") {
    return make_error(ErrorCode::invalid_argument, "store name may not be a directory reference", std::string(name));
  }
  for (char raw : name) {
    const bool safe = (raw >= 'a' && raw <= 'z') || (raw >= 'A' && raw <= 'Z') ||
                      (raw >= '0' && raw <= '9') || raw == '.' || raw == '_' || raw == '-';
    if (!safe) {
      return make_error(ErrorCode::invalid_argument,
                        "store name must not contain a path separator or refused character", std::string(name));
    }
  }
  XSR_RETURN_IF_ERROR(platform::ensure_directory(directory));

  RecordStore store;
  store.log_path_ = log_path_for(directory, name);
  store.snapshot_path_ = snapshot_path_for(directory, name);

  // Leftovers from an interrupted atomic write are never authoritative.
  XSR_RETURN_IF_ERROR(platform::remove_file_if_present(std::filesystem::path(store.log_path_.string() + ".tmp")));
  XSR_RETURN_IF_ERROR(
      platform::remove_file_if_present(std::filesystem::path(store.snapshot_path_.string() + ".tmp")));

  if (std::filesystem::exists(store.snapshot_path_)) {
    auto image = platform::read_file_bounded(
        store.snapshot_path_, static_cast<std::uint64_t>(limits::max_snapshot_payload) + store_format::snapshot_header_size);
    if (!image.ok()) return image.error();
    const auto& bytes = image.value();
    std::uint64_t sequence = 0;
    std::uint32_t payload_length = 0;
    XSR_RETURN_IF_ERROR(store_format::validate_snapshot_header(bytes, sequence, payload_length));
    if (bytes.size() != store_format::snapshot_header_size + payload_length) {
      return make_error(ErrorCode::corrupt, "snapshot length does not match its header");
    }
    const std::span<const std::uint8_t> payload(bytes.data() + store_format::snapshot_header_size, payload_length);
    if (crc32c(payload) != store_format::get_u32(bytes, 20)) {
      return make_error(ErrorCode::corrupt, "snapshot payload checksum failed");
    }
    store.has_snapshot_ = true;
    store.snapshot_sequence_ = sequence;
    store.snapshot_bytes_ = bytes.size();
    store.last_sequence_ = sequence;
  }

  const bool log_exists = std::filesystem::exists(store.log_path_);
  std::FILE* file = platform::open_file(store.log_path_.string().c_str(), log_exists ? "r+b" : "w+b");
  if (file == nullptr) {
    return make_error(ErrorCode::io_error, "opening the record log failed", store.log_path_.string());
  }
  store.log_ = file;
  if (!log_exists) {
    const auto header = store_format::make_log_header();
    if (std::fwrite(header.data(), 1, header.size(), file) != header.size()) {
      return make_error(ErrorCode::io_error, "writing the log header failed");
    }
    XSR_RETURN_IF_ERROR(stream_sync(file));
  }

  ScanOutcome outcome;
  outcome.good_bytes = store_format::log_header_size;
  XSR_RETURN_IF_ERROR(
      scan_log(file, store.has_snapshot_ ? store.snapshot_sequence_ : 0, nullptr, true, outcome));

  store.last_sequence_ = std::max(store.last_sequence_, outcome.last_sequence);
  store.next_sequence_ = store.last_sequence_ + 1;
  auto size = stream_size(file);
  if (!size.ok()) return size.error();
  store.log_bytes_ = size.value();
  store.torn_tail_bytes_at_open_ = outcome.torn_tail_bytes;
  store.dirty_ = false;
  if (store.has_snapshot_ && store.log_bytes_ < store_format::log_header_size) {
    return make_error(ErrorCode::corrupt, "log is shorter than its header");
  }
  return store;
}

Result<ReplayReport> RecordStore::replay(const std::function<Status(const Record&)>& apply) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (log_ == nullptr) {
    return make_error(ErrorCode::invalid_state, "store is not open");
  }
  ReplayReport report;
  report.snapshot_sequence = snapshot_sequence_;
  report.has_snapshot = has_snapshot_;
  report.torn_tail_bytes_truncated = torn_tail_bytes_at_open_;
  ScanOutcome outcome;
  outcome.good_bytes = store_format::log_header_size;
  XSR_RETURN_IF_ERROR(scan_log(log_, has_snapshot_ ? snapshot_sequence_ : 0, &apply, false, outcome));
  report.records_applied = outcome.applied;
  report.records_skipped_by_snapshot = outcome.skipped;
  report.last_sequence = std::max(snapshot_sequence_, outcome.last_sequence);
  report.log_bytes = log_bytes_;
  report.snapshot_bytes = snapshot_bytes_;
  return report;
}

Status RecordStore::append(RecordKind kind, std::string_view payload) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (log_ == nullptr) {
    return make_error(ErrorCode::invalid_state, "store is not open");
  }
  if (payload.size() > limits::max_record_payload) {
    return make_error(ErrorCode::too_large, "record payload exceeds the allowed budget");
  }
  const std::uint64_t record_bytes = store_format::record_header_size + payload.size();
  if (log_bytes_ + record_bytes > limits::max_log_bytes) {
    return make_error(ErrorCode::resource_exhausted, "record log would exceed its size budget");
  }
  auto header = store_format::make_record_header(static_cast<std::uint32_t>(payload.size()), kind, next_sequence_);
  store_format::put_u32(header, 16, crc32c(std::string_view(payload)));
  store_format::put_u32(header, 20, crc32c(std::span<const std::uint8_t>(header).subspan(0, 20)));
  if (stream_seek(log_, static_cast<std::int64_t>(log_bytes_)) != 0) {
    return make_error(ErrorCode::io_error, "seeking to the end of the log failed");
  }
  if (std::fwrite(header.data(), 1, header.size(), log_) != header.size()) {
    return make_error(ErrorCode::io_error, "writing a record header failed");
  }
  if (!payload.empty()) {
    if (std::fwrite(payload.data(), 1, payload.size(), log_) != payload.size()) {
      return make_error(ErrorCode::io_error, "writing a record payload failed");
    }
  }
  log_bytes_ += record_bytes;
  last_sequence_ = next_sequence_;
  ++next_sequence_;
  dirty_ = true;
  return ok_status();
}

Status RecordStore::commit() {
  std::lock_guard<std::mutex> guard(mutex_);
  if (log_ == nullptr) {
    return make_error(ErrorCode::invalid_state, "store is not open");
  }
  return flush_locked();
}

Status RecordStore::flush_locked() {
  if (!dirty_) return ok_status();
  XSR_RETURN_IF_ERROR(stream_sync(log_));
  dirty_ = false;
  return ok_status();
}

Status RecordStore::append_commit(RecordKind kind, std::string_view payload) {
  XSR_RETURN_IF_ERROR(append(kind, payload));
  return commit();
}

Status RecordStore::compact(std::string_view snapshot_payload) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (log_ == nullptr) {
    return make_error(ErrorCode::invalid_state, "store is not open");
  }
  if (dirty_) {
    return make_error(ErrorCode::invalid_state,
                      "compaction requires a committed log; uncommitted state may not be discarded");
  }
  if (snapshot_payload.size() > limits::max_snapshot_payload) {
    return make_error(ErrorCode::too_large, "snapshot payload exceeds the allowed budget");
  }
  const std::uint64_t image_bytes = store_format::log_header_size;

  {
    std::vector<std::uint8_t> image(store_format::snapshot_header_size + snapshot_payload.size(), 0);
    auto header = store_format::make_snapshot_header(last_sequence_, static_cast<std::uint32_t>(snapshot_payload.size()));
    std::memcpy(image.data(), header.data(), header.size());
    if (!snapshot_payload.empty()) {
      std::memcpy(image.data() + store_format::snapshot_header_size, snapshot_payload.data(), snapshot_payload.size());
    }
    const std::span<const std::uint8_t> payload(image.data() + store_format::snapshot_header_size, snapshot_payload.size());
    store_format::put_u32(image, 20, crc32c(payload));
    store_format::put_u32(image, 24, crc32c(std::span<const std::uint8_t>(image).subspan(0, 24)));
    XSR_RETURN_IF_ERROR(platform::write_file_atomic(snapshot_path_, image, true));
  }
  {
    // The log handle must be released before the file is replaced: on Windows an
    // open handle without FILE_SHARE_DELETE blocks the rename.
    std::fclose(log_);
    log_ = nullptr;
    const std::vector<std::uint8_t> image = store_format::make_log_header();
    Status written = platform::write_file_atomic(log_path_, image, true);
    log_ = platform::open_file(log_path_.string().c_str(), "r+b");
    if (log_ == nullptr) {
      return make_error(ErrorCode::io_error, "reopening the compacted log failed", log_path_.string());
    }
    XSR_RETURN_IF_ERROR(written);
  }

  has_snapshot_ = true;
  snapshot_sequence_ = last_sequence_;
  snapshot_bytes_ = store_format::snapshot_header_size + snapshot_payload.size();
  next_sequence_ = last_sequence_ + 1;
  log_bytes_ = image_bytes;
  dirty_ = false;
  return ok_status();
}

Result<std::vector<std::uint8_t>> RecordStore::load_snapshot() const {
  if (!has_snapshot_) return std::vector<std::uint8_t>{};
  auto image = platform::read_file_bounded(
      snapshot_path_, static_cast<std::uint64_t>(limits::max_snapshot_payload) + store_format::snapshot_header_size);
  if (!image.ok()) return image.error();
  const auto& bytes = image.value();
  std::uint64_t sequence = 0;
  std::uint32_t payload_length = 0;
  XSR_RETURN_IF_ERROR(store_format::validate_snapshot_header(bytes, sequence, payload_length));
  if (bytes.size() != store_format::snapshot_header_size + payload_length) {
    return make_error(ErrorCode::corrupt, "snapshot length does not match its header");
  }
  const std::span<const std::uint8_t> payload(bytes.data() + store_format::snapshot_header_size, payload_length);
  if (crc32c(payload) != store_format::get_u32(bytes, 20)) {
    return make_error(ErrorCode::corrupt, "snapshot payload checksum failed");
  }
  return std::vector<std::uint8_t>(payload.begin(), payload.end());
}

std::uint64_t RecordStore::last_sequence() const noexcept {
  std::lock_guard<std::mutex> guard(mutex_);
  return last_sequence_;
}

std::uint64_t RecordStore::next_sequence() const noexcept {
  std::lock_guard<std::mutex> guard(mutex_);
  return next_sequence_;
}

std::uint64_t RecordStore::log_bytes() const noexcept {
  std::lock_guard<std::mutex> guard(mutex_);
  return log_bytes_;
}

std::uint64_t RecordStore::snapshot_sequence() const noexcept {
  std::lock_guard<std::mutex> guard(mutex_);
  return snapshot_sequence_;
}

bool RecordStore::has_snapshot() const noexcept {
  std::lock_guard<std::mutex> guard(mutex_);
  return has_snapshot_;
}

bool RecordStore::is_open() const noexcept {
  std::lock_guard<std::mutex> guard(mutex_);
  return log_ != nullptr;
}

std::filesystem::path RecordStore::log_path() const { return log_path_; }
std::filesystem::path RecordStore::snapshot_path() const { return snapshot_path_; }

Status RecordStore::close() {
  std::lock_guard<std::mutex> guard(mutex_);
  if (log_ == nullptr) return ok_status();
  Status result = flush_locked();
  std::fclose(log_);
  log_ = nullptr;
  return result;
}

Result<ReplayReport> inspect_store(const std::filesystem::path& directory, std::string_view name,
                                   const std::function<Status(const Record&)>& apply) {
  ReplayReport report;
  const auto log_path = log_path_for(directory, name);
  const auto snapshot_path = snapshot_path_for(directory, name);
  std::uint64_t snapshot_sequence = 0;
  bool has_snapshot = false;
  if (std::filesystem::exists(snapshot_path)) {
    auto image = platform::read_file_bounded(
        snapshot_path, static_cast<std::uint64_t>(limits::max_snapshot_payload) + store_format::snapshot_header_size);
    if (!image.ok()) return image.error();
    std::uint64_t sequence = 0;
    std::uint32_t payload_length = 0;
    XSR_RETURN_IF_ERROR(store_format::validate_snapshot_header(image.value(), sequence, payload_length));
    const auto& bytes = image.value();
    if (bytes.size() != store_format::snapshot_header_size + payload_length) {
      return make_error(ErrorCode::corrupt, "snapshot length does not match its header");
    }
    const std::span<const std::uint8_t> payload(bytes.data() + store_format::snapshot_header_size, payload_length);
    if (crc32c(payload) != store_format::get_u32(bytes, 20)) {
      return make_error(ErrorCode::corrupt, "snapshot payload checksum failed");
    }
    snapshot_sequence = sequence;
    has_snapshot = true;
    report.snapshot_sequence = sequence;
    report.has_snapshot = true;
    report.snapshot_bytes = bytes.size();
  }
  if (!std::filesystem::exists(log_path)) {
    return make_error(ErrorCode::not_found, "record log does not exist", log_path.string());
  }
  std::FILE* file = platform::open_file(log_path.string().c_str(), "rb");
  if (file == nullptr) {
    return make_error(ErrorCode::io_error, "opening the record log failed", log_path.string());
  }
  ScanOutcome outcome;
  outcome.good_bytes = store_format::log_header_size;
  Status scanned = scan_log(file, has_snapshot ? snapshot_sequence : 0, &apply, false, outcome);
  auto size = stream_size(file);
  std::fclose(file);
  if (!scanned.ok()) return scanned.error();
  report.records_applied = outcome.applied;
  report.records_skipped_by_snapshot = outcome.skipped;
  report.last_sequence = std::max(snapshot_sequence, outcome.last_sequence);
  report.torn_tail_bytes_truncated = outcome.torn_tail_bytes;
  report.log_bytes = size.ok() ? size.value() : 0;
  return report;
}

}  // namespace xsr
