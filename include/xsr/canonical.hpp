// Cross-Site Reservation runtime — canonical deterministic encoding.
// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xsr/digest.hpp"
#include "xsr/error.hpp"
#include "xsr/limits.hpp"

namespace xsr {

/// Canonical grammar (canonical_version 1):
///   value  := field*
///   field  := name "=" escaped LF
///   name   := [a-z][a-z0-9_]{0,63}
///   escaped := byte* where a byte is literal iff it is in [A-Za-z0-9._:/@+-],
///              otherwise "%" followed by two uppercase hex digits.
///
/// The encoding is injective, byte-exact on every platform, and stable across
/// compilers: digests over canonical bytes are reproducible evidence.
/// Re-encoding an unescaped value must reproduce the original bytes exactly;
/// non-canonical spellings such as %41 for "A" are refused on read.

/// Escapes a value into canonical form.
std::string canonical_escape(std::string_view value);
/// Reverses canonical_escape; refuses non-canonical escapes.
Result<std::string> canonical_unescape(std::string_view text, std::size_t max_length);
/// True when the spelling is already exactly canonical.
bool is_canonical_escaped(std::string_view text, std::size_t max_length = limits::max_canonical_value_length);
/// True when the text is a well-formed field name.
bool is_canonical_name(std::string_view name);

/// Streaming canonical writer. Field order is caller-controlled and must be
/// identical for equal values. Failures are sticky and reported by finish().
class CanonicalWriter {
 public:
  CanonicalWriter() = default;

  void put(std::string_view name, std::string_view value);
  void put_u64(std::string_view name, std::uint64_t value);
  void put_i64(std::string_view name, std::int64_t value);
  void put_bool(std::string_view name, bool value);
  void put_hex(std::string_view name, std::span<const std::uint8_t> bytes);
  void put_digest(std::string_view name, const Digest& digest);
  /// Writes an already-canonical field sequence verbatim (used for nested records).
  void put_block(std::string_view name, const std::string& canonical_block);

  bool failed() const noexcept { return failed_; }
  const Error& error() const noexcept { return error_; }
  std::size_t field_count() const noexcept { return fields_; }
  bool empty() const noexcept { return buf_.empty(); }
  const std::string& str() const noexcept { return buf_; }
  std::span<const std::uint8_t> bytes() const noexcept {
    return {reinterpret_cast<const std::uint8_t*>(buf_.data()), buf_.size()};
  }

  /// Validates bounds and returns the encoded bytes. Must be called before use.
  Status finish() const;

 private:
  void fail(ErrorCode code, std::string message);

  std::string buf_;
  std::size_t fields_ = 0;
  bool failed_ = false;
  Error error_;
};

/// Strict canonical reader. Fields must be taken in the exact order written.
class CanonicalReader {
 public:
  struct Field {
    std::string name;
    std::string value;  // unescaped
  };

  static Result<CanonicalReader> parse(std::span<const std::uint8_t> data);
  static Result<CanonicalReader> parse(std::string_view data);

  /// Next field must be named \p name.
  Result<std::string> take(std::string_view name);
  Result<std::optional<std::string>> take_optional(std::string_view name);
  Result<std::uint64_t> take_u64(std::string_view name);
  Result<std::int64_t> take_i64(std::string_view name);
  Result<bool> take_bool(std::string_view name);
  Result<Digest> take_digest(std::string_view name);
  Result<std::vector<std::uint8_t>> take_hex(std::string_view name, std::size_t max_bytes);
  Result<std::string> take_block(std::string_view name, std::size_t max_bytes);

  /// Fails unless every field was consumed.
  Status finish() const;
  std::size_t remaining() const noexcept { return fields_.size() - index_; }
  std::size_t consumed() const noexcept { return index_; }
  const std::vector<Field>& fields() const noexcept { return fields_; }
  bool empty() const noexcept { return fields_.empty(); }

 private:
  Result<const Field*> peek(std::string_view name);

  std::vector<Field> fields_;
  std::size_t index_ = 0;
};

/// Strict decimal parser: no sign, no whitespace, no leading zeros.
Result<std::uint64_t> parse_u64(std::string_view text);
/// Strict signed decimal parser: optional '-', no '+', no whitespace, no leading zeros.
Result<std::int64_t> parse_i64(std::string_view text);

}  // namespace xsr
