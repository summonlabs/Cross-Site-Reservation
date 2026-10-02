// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#include <cstdio>
#include <string>
#include <vector>

#include "framework.hpp"
#include "support/harness.hpp"
#include "xsr/platform.hpp"
#include "xsr/store.hpp"

using namespace xsr;

namespace {

constexpr std::size_t kLogHeader = store_format::log_header_size;
constexpr std::size_t kRecordHeader = store_format::record_header_size;

struct TempStore {
  std::filesystem::path root = xsrtest::make_temp_root("store");
  ~TempStore() { xsrtest::remove_tree(root); }
  std::filesystem::path log() const { return root / "state.xsrlog"; }
  std::filesystem::path snapshot() const { return root / "state.xsrsnap"; }
};

std::vector<std::uint8_t> read_all(const std::filesystem::path& path) {
  std::vector<std::uint8_t> out;
  std::FILE* file = platform::open_file(path.string().c_str(), "rb");
  if (file == nullptr) return out;
  std::uint8_t buffer[4096];
  std::size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    out.insert(out.end(), buffer, buffer + got);
  }
  std::fclose(file);
  return out;
}

void write_all(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
  std::FILE* file = platform::open_file(path.string().c_str(), "wb");
  if (file == nullptr) return;
  if (!bytes.empty()) std::fwrite(bytes.data(), 1, bytes.size(), file);
  std::fclose(file);
}

void append_raw(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
  std::FILE* file = platform::open_file(path.string().c_str(), "ab");
  if (file == nullptr) return;
  if (!bytes.empty()) std::fwrite(bytes.data(), 1, bytes.size(), file);
  std::fclose(file);
}

std::uint64_t bytes_on_disk(const std::filesystem::path& path) {
  std::error_code error;
  return static_cast<std::uint64_t>(std::filesystem::file_size(path, error));
}

/// Offset of record \p index given a uniform payload width.
std::size_t record_offset(std::size_t index, std::size_t payload_width) {
  return kLogHeader + index * (kRecordHeader + payload_width);
}

Status collect(std::vector<Record>* out, const Record& record) {
  out->push_back(record);
  return ok_status();
}

std::string payload_for(std::size_t index) {
  std::string text = "payload" + std::to_string(index);
  while (text.size() < 8) text.push_back('.');
  return text.substr(0, 8);
}

/// Rewrites the header checksum after an intentional header mutation.
void repair_record_header_crc(std::vector<std::uint8_t>& bytes, std::size_t offset) {
  const std::span<std::uint8_t> header(bytes.data() + offset, kRecordHeader);
  store_format::put_u32(header, 20, crc32c(std::span<const std::uint8_t>(header).subspan(0, 20)));
}

}  // namespace

XSR_TEST(store, append_commit_and_replay) {
  TempStore temp;
  {
    auto store = RecordStore::open(temp.root, "state");
    XSR_CHECK_OK(store);
    for (std::size_t index = 0; index < 3; ++index) {
      XSR_CHECK_OK(store.value().append(RecordKind::site_prepare, payload_for(index)));
      XSR_CHECK_EQ(store.value().log_bytes() > kLogHeader, true);
    }
    XSR_CHECK_OK(store.value().commit());
    XSR_CHECK_EQ(store.value().last_sequence(), 3ull);
    XSR_CHECK_OK(store.value().append_commit(RecordKind::site_commit, payload_for(3)));
    XSR_CHECK_EQ(store.value().last_sequence(), 4ull);
    XSR_CHECK_OK(store.value().close());
  }
  auto reopened = RecordStore::open(temp.root, "state");
  XSR_CHECK_OK(reopened);
  std::vector<Record> records;
  auto report = reopened.value().replay([&records](const Record& record) { return collect(&records, record); });
  XSR_CHECK_OK(report);
  XSR_CHECK_EQ(records.size(), std::size_t{4});
  XSR_CHECK_EQ(report.value().last_sequence, 4ull);
  XSR_CHECK_EQ(report.value().records_applied, std::size_t{4});
  XSR_CHECK_EQ(report.value().records_skipped_by_snapshot, std::size_t{0});
  XSR_CHECK_EQ(report.value().torn_tail_bytes_truncated, 0ull);
  for (std::size_t index = 0; index < records.size(); ++index) {
    XSR_CHECK_EQ(records[index].sequence, index + 1);
    XSR_CHECK_EQ(records[index].payload, payload_for(index));
  }
  XSR_CHECK_EQ(records[0].kind, RecordKind::site_prepare);
  XSR_CHECK_EQ(records[3].kind, RecordKind::site_commit);
}

XSR_TEST(store, torn_tail_is_detected_and_removed) {
  TempStore temp;
  {
    auto store = RecordStore::open(temp.root, "state");
    XSR_CHECK_OK(store);
    for (std::size_t index = 0; index < 2; ++index) {
      XSR_CHECK_OK(store.value().append_commit(RecordKind::site_prepare, payload_for(index)));
    }
    XSR_CHECK_OK(store.value().close());
  }
  const std::uint64_t clean_size = bytes_on_disk(temp.log());
  // A complete header announcing a payload that never arrives.
  auto header = store_format::make_record_header(64, RecordKind::site_prepare, 3);
  store_format::put_u32(header, 16, crc32c(std::string_view("short")));
  repair_record_header_crc(header, 0);
  header.insert(header.end(), {'s', 'h', 'o', 'r', 't'});
  append_raw(temp.log(), header);

  auto store = RecordStore::open(temp.root, "state");
  XSR_CHECK_OK(store);
  std::vector<Record> records;
  auto report = store.value().replay([&records](const Record& record) { return collect(&records, record); });
  XSR_CHECK_OK(report);
  XSR_CHECK_EQ(records.size(), std::size_t{2});
  XSR_CHECK_EQ(report.value().torn_tail_bytes_truncated, std::uint64_t{header.size()});
  XSR_CHECK_EQ(store.value().last_sequence(), 2ull);
  XSR_CHECK_OK(store.value().close());
  XSR_CHECK_EQ(bytes_on_disk(temp.log()), clean_size);
}

XSR_TEST(store, zero_filled_tail_is_a_torn_tail) {
  TempStore temp;
  {
    auto store = RecordStore::open(temp.root, "state");
    XSR_CHECK_OK(store);
    XSR_CHECK_OK(store.value().append_commit(RecordKind::site_prepare, payload_for(0)));
    XSR_CHECK_OK(store.value().close());
  }
  const std::uint64_t clean_size = bytes_on_disk(temp.log());
  std::error_code error;
  std::filesystem::resize_file(temp.log(), clean_size + 64 * 1024, error);
  XSR_CHECK_EQ(error.value(), 0);
  auto store = RecordStore::open(temp.root, "state");
  XSR_CHECK_OK(store);
  std::vector<Record> records;
  auto report = store.value().replay([&records](const Record& record) { return collect(&records, record); });
  XSR_CHECK_OK(report);
  XSR_CHECK_EQ(records.size(), std::size_t{1});
  XSR_CHECK_EQ(report.value().torn_tail_bytes_truncated, 64ull * 1024ull);
  XSR_CHECK_OK(store.value().close());
  XSR_CHECK_EQ(bytes_on_disk(temp.log()), clean_size);
}

XSR_TEST(store, interior_payload_corruption_refuses_to_open) {
  TempStore temp;
  {
    auto store = RecordStore::open(temp.root, "state");
    XSR_CHECK_OK(store);
    for (std::size_t index = 0; index < 3; ++index) {
      XSR_CHECK_OK(store.value().append_commit(RecordKind::site_prepare, payload_for(index)));
    }
    XSR_CHECK_OK(store.value().close());
  }
  auto bytes = read_all(temp.log());
  bytes[record_offset(1, 8) + kRecordHeader] = static_cast<std::uint8_t>('X');
  write_all(temp.log(), bytes);
  XSR_CHECK_ERR(RecordStore::open(temp.root, "state"), ErrorCode::corrupt);
}

XSR_TEST(store, interior_header_corruption_refuses_to_open) {
  TempStore temp;
  {
    auto store = RecordStore::open(temp.root, "state");
    XSR_CHECK_OK(store);
    for (std::size_t index = 0; index < 3; ++index) {
      XSR_CHECK_OK(store.value().append_commit(RecordKind::site_prepare, payload_for(index)));
    }
    XSR_CHECK_OK(store.value().close());
  }
  auto bytes = read_all(temp.log());
  store_format::put_u64(std::span<std::uint8_t>(bytes.data() + record_offset(1, 8), kRecordHeader), 8, 99);
  write_all(temp.log(), bytes);
  XSR_CHECK_ERR(RecordStore::open(temp.root, "state"), ErrorCode::corrupt);
}

XSR_TEST(store, sequence_gap_refuses_to_open) {
  TempStore temp;
  {
    auto store = RecordStore::open(temp.root, "state");
    XSR_CHECK_OK(store);
    for (std::size_t index = 0; index < 3; ++index) {
      XSR_CHECK_OK(store.value().append_commit(RecordKind::site_prepare, payload_for(index)));
    }
    XSR_CHECK_OK(store.value().close());
  }
  auto bytes = read_all(temp.log());
  const std::span<std::uint8_t> header(bytes.data() + record_offset(1, 8), kRecordHeader);
  store_format::put_u64(header, 8, 7);
  store_format::put_u32(header, 20, crc32c(std::span<const std::uint8_t>(header).subspan(0, 20)));
  write_all(temp.log(), bytes);
  XSR_CHECK_ERR(RecordStore::open(temp.root, "state"), ErrorCode::corrupt);
}

XSR_TEST(store, absurd_payload_length_is_refused_before_allocation) {
  TempStore temp;
  {
    auto store = RecordStore::open(temp.root, "state");
    XSR_CHECK_OK(store);
    for (std::size_t index = 0; index < 2; ++index) {
      XSR_CHECK_OK(store.value().append_commit(RecordKind::site_prepare, payload_for(index)));
    }
    XSR_CHECK_OK(store.value().close());
  }
  auto bytes = read_all(temp.log());
  const std::span<std::uint8_t> header(bytes.data() + record_offset(1, 8), kRecordHeader);
  store_format::put_u32(header, 0, 0x7FFFFFFFu);
  store_format::put_u32(header, 20, crc32c(std::span<const std::uint8_t>(header).subspan(0, 20)));
  write_all(temp.log(), bytes);
  XSR_CHECK_ERR(RecordStore::open(temp.root, "state"), ErrorCode::corrupt);
}

XSR_TEST(store, unknown_record_kind_is_refused) {
  TempStore temp;
  {
    auto store = RecordStore::open(temp.root, "state");
    XSR_CHECK_OK(store);
    XSR_CHECK_OK(store.value().append_commit(RecordKind::site_prepare, payload_for(0)));
    XSR_CHECK_OK(store.value().close());
  }
  auto bytes = read_all(temp.log());
  const std::span<std::uint8_t> header(bytes.data() + record_offset(0, 8), kRecordHeader);
  store_format::put_u32(header, 4, 9999);
  store_format::put_u32(header, 20, crc32c(std::span<const std::uint8_t>(header).subspan(0, 20)));
  write_all(temp.log(), bytes);
  XSR_CHECK_ERR(RecordStore::open(temp.root, "state"), ErrorCode::corrupt);
}

XSR_TEST(store, corrupted_or_unsupported_file_header) {
  TempStore temp;
  {
    auto store = RecordStore::open(temp.root, "state");
    XSR_CHECK_OK(store);
    XSR_CHECK_OK(store.value().append_commit(RecordKind::site_prepare, payload_for(0)));
    XSR_CHECK_OK(store.value().close());
  }
  {
    auto bytes = read_all(temp.log());
    bytes[0] = static_cast<std::uint8_t>('Z');
    write_all(temp.log(), bytes);
    XSR_CHECK_ERR(RecordStore::open(temp.root, "state"), ErrorCode::corrupt);
  }
  temp.root /= "";
  {
    // Restore a valid header, then claim an unsupported format version.
    std::vector<std::uint8_t> fresh = store_format::make_log_header();
    append_raw(temp.log(), {});
    write_all(temp.log(), fresh);
    auto store = RecordStore::open(temp.root, "state");
    XSR_CHECK_OK(store);
    for (std::size_t index = 0; index < 2; ++index) {
      XSR_CHECK_OK(store.value().append_commit(RecordKind::site_prepare, payload_for(index)));
    }
    XSR_CHECK_OK(store.value().close());
  }
  {
    auto bytes = read_all(temp.log());
    bytes[4] = 9;
    bytes[5] = 0;
    write_all(temp.log(), bytes);
    XSR_CHECK_ERR(RecordStore::open(temp.root, "state"), ErrorCode::version_mismatch);
  }
}

XSR_TEST(store, truncated_file_is_refused) {
  TempStore temp;
  {
    auto store = RecordStore::open(temp.root, "state");
    XSR_CHECK_OK(store);
    XSR_CHECK_OK(store.value().append_commit(RecordKind::site_prepare, payload_for(0)));
    XSR_CHECK_OK(store.value().close());
  }
  auto bytes = read_all(temp.log());
  bytes.resize(10);
  write_all(temp.log(), bytes);
  XSR_CHECK_ERR(RecordStore::open(temp.root, "state"), ErrorCode::corrupt);
}

XSR_TEST(store, compaction_folds_state_into_a_snapshot) {
  TempStore temp;
  {
    auto store = RecordStore::open(temp.root, "state");
    XSR_CHECK_OK(store);
    for (std::size_t index = 0; index < 5; ++index) {
      XSR_CHECK_OK(store.value().append_commit(RecordKind::site_prepare, payload_for(index)));
    }
    XSR_CHECK_OK(store.value().compact("format=1\nentry_count=5\n"));
    XSR_CHECK_EQ(store.value().has_snapshot(), true);
    XSR_CHECK_EQ(store.value().snapshot_sequence(), 5ull);
    XSR_CHECK_EQ(store.value().last_sequence(), 5ull);
    XSR_CHECK_EQ(store.value().log_bytes(), std::uint64_t{kLogHeader});
    XSR_CHECK_OK(store.value().append_commit(RecordKind::site_commit, payload_for(5)));
    XSR_CHECK_EQ(store.value().last_sequence(), 6ull);
    XSR_CHECK_OK(store.value().close());
  }
  auto reopened = RecordStore::open(temp.root, "state");
  XSR_CHECK_OK(reopened);
  std::vector<Record> records;
  auto report = reopened.value().replay([&records](const Record& record) { return collect(&records, record); });
  XSR_CHECK_OK(report);
  XSR_CHECK_EQ(records.size(), std::size_t{1});
  XSR_CHECK_EQ(records[0].sequence, 6ull);
  XSR_CHECK_EQ(report.value().records_skipped_by_snapshot, std::size_t{0});
  auto snapshot = reopened.value().load_snapshot();
  XSR_CHECK_OK(snapshot);
  const std::string image(snapshot.value().begin(), snapshot.value().end());
  XSR_CHECK_EQ(image, std::string("format=1\nentry_count=5\n"));
}

XSR_TEST(store, compaction_refuses_uncommitted_state) {
  TempStore temp;
  auto store = RecordStore::open(temp.root, "state");
  XSR_CHECK_OK(store);
  XSR_CHECK_OK(store.value().append(RecordKind::site_prepare, payload_for(0)));
  XSR_CHECK_ERR(store.value().compact("format=1\n"), ErrorCode::invalid_state);
  XSR_CHECK_OK(store.value().commit());
  XSR_CHECK_OK(store.value().compact("format=1\n"));
}

XSR_TEST(store, stale_log_records_are_skipped_when_the_snapshot_is_newer) {
  TempStore temp;
  {
    auto store = RecordStore::open(temp.root, "state");
    XSR_CHECK_OK(store);
    for (std::size_t index = 0; index < 3; ++index) {
      XSR_CHECK_OK(store.value().append_commit(RecordKind::site_prepare, payload_for(index)));
    }
    // Write the snapshot directly, leaving the log untouched: this is exactly the
    // state a crash between the two compaction writes leaves behind.
    auto header = store_format::make_snapshot_header(3, 9);
    std::vector<std::uint8_t> image = header;
    const std::string payload = "format=1\n";
    image.insert(image.end(), payload.begin(), payload.end());
    store_format::put_u32(image, 16, static_cast<std::uint32_t>(payload.size()));
    store_format::put_u32(image, 20, crc32c(std::string_view(payload)));
    store_format::put_u32(image, 24, crc32c(std::span<const std::uint8_t>(image).subspan(0, 24)));
    write_all(temp.snapshot(), image);
    XSR_CHECK_OK(store.value().close());
  }
  auto reopened = RecordStore::open(temp.root, "state");
  XSR_CHECK_OK(reopened);
  std::vector<Record> records;
  auto report = reopened.value().replay([&records](const Record& record) { return collect(&records, record); });
  XSR_CHECK_OK(report);
  XSR_CHECK_EQ(records.size(), std::size_t{0});
  XSR_CHECK_EQ(report.value().records_skipped_by_snapshot, std::size_t{3});
  XSR_CHECK_EQ(report.value().last_sequence, 3ull);
  XSR_CHECK_OK(reopened.value().append_commit(RecordKind::site_prepare, payload_for(3)));
  XSR_CHECK_EQ(reopened.value().last_sequence(), 4ull);
}

XSR_TEST(store, corrupted_snapshot_refuses_to_open) {
  TempStore temp;
  {
    auto store = RecordStore::open(temp.root, "state");
    XSR_CHECK_OK(store);
    XSR_CHECK_OK(store.value().append_commit(RecordKind::site_prepare, payload_for(0)));
    XSR_CHECK_OK(store.value().compact("format=1\n"));
    XSR_CHECK_OK(store.value().close());
  }
  {
    auto bytes = read_all(temp.snapshot());
    bytes[10] = static_cast<std::uint8_t>(bytes[10] ^ 0xFFu);
    write_all(temp.snapshot(), bytes);
    XSR_CHECK_ERR(RecordStore::open(temp.root, "state"), ErrorCode::corrupt);
  }
}

XSR_TEST(store, stop_words_larger_than_the_budget_are_refused) {
  TempStore temp;
  auto store = RecordStore::open(temp.root, "state");
  XSR_CHECK_OK(store);
  const std::string huge(limits::max_record_payload + 1, 'x');
  XSR_CHECK_ERR(store.value().append(RecordKind::site_prepare, huge), ErrorCode::too_large);
  XSR_CHECK_ERR(store.value().append_commit(RecordKind::site_prepare, huge), ErrorCode::too_large);
}

XSR_TEST(store, inspect_verifies_without_truncating) {
  TempStore temp;
  {
    auto store = RecordStore::open(temp.root, "state");
    XSR_CHECK_OK(store);
    XSR_CHECK_OK(store.value().append_commit(RecordKind::site_prepare, payload_for(0)));
    XSR_CHECK_OK(store.value().close());
  }
  const std::uint64_t clean_size = bytes_on_disk(temp.log());
  auto header = store_format::make_record_header(4, RecordKind::site_prepare, 2);
  repair_record_header_crc(header, 0);
  header.insert(header.end(), {'a', 'b'});
  append_raw(temp.log(), header);
  const std::uint64_t torn_size = bytes_on_disk(temp.log());
  std::size_t applied = 0;
  auto report = inspect_store(temp.root, "state", [&applied](const Record&) {
    ++applied;
    return ok_status();
  });
  XSR_CHECK_OK(report);
  XSR_CHECK_EQ(applied, std::size_t{1});
  XSR_CHECK_EQ(report.value().torn_tail_bytes_truncated, std::uint64_t{header.size()});
  XSR_CHECK_EQ(bytes_on_disk(temp.log()), torn_size);
  XSR_CHECK(true);
  (void)clean_size;
}

XSR_TEST(store, missing_store_is_reported) {
  TempStore temp;
  XSR_CHECK_ERR(inspect_store(temp.root, "absent", nullptr), ErrorCode::not_found);
}

XSR_TEST(store, names_cannot_escape_the_state_directory) {
  TempStore temp;
  XSR_CHECK_ERR(RecordStore::open(temp.root, "../escape"), ErrorCode::invalid_argument);
  XSR_CHECK_ERR(RecordStore::open(temp.root, "dir/name"), ErrorCode::invalid_argument);
  XSR_CHECK_ERR(RecordStore::open(temp.root, ".."), ErrorCode::invalid_argument);
  XSR_CHECK_ERR(RecordStore::open(temp.root, ""), ErrorCode::invalid_argument);
  XSR_CHECK_ERR(RecordStore::open(temp.root, std::string(65, 'a')), ErrorCode::invalid_argument);
  XSR_CHECK_OK(RecordStore::open(temp.root, "state-1.0"));
}

XSR_TEST(store, committed_survives_reopen_after_many_appends) {
  TempStore temp;
  {
    auto store = RecordStore::open(temp.root, "state");
    XSR_CHECK_OK(store);
    for (std::size_t index = 0; index < 200; ++index) {
      XSR_CHECK_OK(store.value().append_commit(RecordKind::site_prepare, payload_for(index % 97)));
    }
    XSR_CHECK_OK(store.value().compact("format=1\nbulk=1\n"));
    for (std::size_t index = 0; index < 50; ++index) {
      XSR_CHECK_OK(store.value().append_commit(RecordKind::site_commit, payload_for(index % 97)));
    }
    XSR_CHECK_OK(store.value().close());
  }
  auto reopened = RecordStore::open(temp.root, "state");
  XSR_CHECK_OK(reopened);
  XSR_CHECK_EQ(reopened.value().last_sequence(), 250ull);
  std::size_t applied = 0;
  auto report = reopened.value().replay([&applied](const Record&) {
    ++applied;
    return ok_status();
  });
  XSR_CHECK_OK(report);
  XSR_CHECK_EQ(applied, std::size_t{50});
}
